#include "Dimension/Image/ImageGpu.h"
#include "Dimension/Image/ImageFilters.h"
#include "Dimension/Renderer/Buffer.h"
#include "Dimension/Renderer/Pipeline.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "shaders/ImageBoxBlur.comp.h"
#include "shaders/ImageComposite.comp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace Dimension::Imaging::Gpu
{
    namespace
    {
        // One operation of ImageComposite.comp (its Op struct, std430: 64 bytes).
        struct Op
        {
            uint32_t Kind = 0, Mode = 0;
            float Opacity = 1.0f;
            uint32_t Flags = 0;
            uint64_t Pixels = 0, Mask = 0, Aux = 0;
            uint32_t AdjustType = 0, Pad = 0;
            float Color[4] = { 0, 0, 0, 0 };
        };
        static_assert(sizeof(Op) == 64);

        struct Resident
        {
            std::shared_ptr<const void> Hold; // (the data stays alive, so its address is not reused)
            Buffer Buf;
            VkDeviceAddress Address = 0;
            bool Used = false;
        };

        struct State
        {
            std::unordered_map<const void*, Resident> Buffers;
            Buffer Ops, Params, Out, BlurA, BlurB;
            Pipeline Composite, Blur;
            bool Ready = false;
            double LastMs = 0.0;
        };
        // (never destroyed: its buffers and pipelines must go before the device does, in Release, not at exit)
        State& S()
        {
            static State* s = new State;
            return *s;
        }

        constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        VkDeviceAddress AddressOf(const Buffer& b)
        {
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = b.GetHandle();
            return vkGetBufferDeviceAddress(RenderDevice::Get().GetDevice(), &info);
        }

        // A mapped buffer of at least `bytes` (made again, larger, when too small).
        void Ensure(Buffer& b, size_t bytes, MemoryUsage memory, const char* name)
        {
            bytes = std::max<size_t>(16, (bytes + 15) & ~size_t(15));
            if (b.IsValid() && b.GetSize() >= bytes) return;
            b = Buffer(bytes + bytes / 4, kUsage, memory, name);
        }

        bool Init()
        {
            State& s = S();
            if (s.Ready) return true;
            if (!RenderDevice::Exists()) return false;
            s.Composite = Pipeline::Compute(kImageComposite_comp, "ImageComposite");
            s.Blur = Pipeline::Compute(kImageBoxBlur_comp, "ImageBoxBlur");
            s.Ready = true;
            return true;
        }

        // Data on the GPU (uploaded once while it lives; `bytes` of it).
        VkDeviceAddress Upload(const std::shared_ptr<const void>& hold, const void* data, size_t bytes)
        {
            State& s = S();
            auto it = s.Buffers.find(data);
            if (it != s.Buffers.end())
            {
                it->second.Used = true;
                return it->second.Address;
            }
            Resident r;
            r.Hold = hold;
            size_t size = std::max<size_t>(16, (bytes + 15) & ~size_t(15));
            r.Buf = Buffer(size, kUsage, MemoryUsage::CpuToGpu, "ImageLayer");
            std::memcpy(r.Buf.GetMapped(), data, bytes);
            if (size > bytes) std::memset((uint8_t*)r.Buf.GetMapped() + bytes, 0, size - bytes);
            r.Address = AddressOf(r.Buf);
            r.Used = true;
            VkDeviceAddress a = r.Address;
            s.Buffers.emplace(data, std::move(r));
            return a;
        }

        void Barrier(VkCommandBuffer cmd, VkAccessFlags2 dstAccess, VkPipelineStageFlags2 dstStage)
        {
            VkMemoryBarrier2 b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // The document's layers as operations, in the CPU compositor's order.
        struct Builder
        {
            const Document& Doc;
            std::unordered_map<uint32_t, std::vector<int>> Kids;
            std::vector<Op> Ops;
            std::vector<float> Params;
            std::vector<size_t> ParamOps; // (ops whose Aux is an offset into Params, made an address once it is up)
            int Depth = 0, MaxDepth = 0;

            void Mask(Op& op, const Layer& l)
            {
                if (!l.LayerMask || !l.MaskEnabled) return;
                op.Flags |= 1u;
                op.Mask = Upload(l.LayerMask, l.LayerMask->Values.data(), l.LayerMask->Values.size());
            }

            void Emit(uint32_t parent)
            {
                auto it = Kids.find(parent);
                if (it == Kids.end()) return;
                for (int index : it->second)
                {
                    const Layer& l = Doc.Layers[(size_t)index];
                    if (!l.Visible || l.Opacity <= 0.0f) continue;
                    if (l.IsGroup())
                    {
                        const bool pass = l.Blend == LayerBlend::PassThrough;
                        Op begin;
                        begin.Kind = pass ? 4u : 2u;
                        Ops.push_back(begin);
                        MaxDepth = std::max(MaxDepth, ++Depth);
                        Emit(l.Id);
                        --Depth;
                        Op end;
                        end.Kind = pass ? 5u : 3u;
                        end.Mode = (uint32_t)l.Blend;
                        end.Opacity = l.Opacity;
                        Mask(end, l);
                        Ops.push_back(end);
                        continue;
                    }
                    if (l.IsAdjustment())
                    {
                        Op op;
                        op.Kind = 6u;
                        op.Mode = (uint32_t)l.Blend;
                        op.Opacity = l.Opacity;
                        op.AdjustType = (uint32_t)l.Adjust.Type;
                        Mask(op, l);
                        PreparedAdjustment p = Prepare(l.Adjust);
                        op.Aux = Params.size();
                        const Adjustment& a = l.Adjust;
                        if (p.UsesLut)
                        {
                            op.Flags |= 2u;
                            for (const auto& ch : p.Lut) Params.insert(Params.end(), ch.begin(), ch.end());
                        }
                        else
                            switch (a.Type)
                            {
                            case AdjustmentType::HueSaturation: Params.insert(Params.end(), { a.Hue, a.Saturation, a.Lightness, a.Colorize ? 1.0f : 0.0f }); break;
                            case AdjustmentType::ColorBalance:
                                for (const glm::vec3& b : a.Balance) Params.insert(Params.end(), { b.x, b.y, b.z });
                                Params.push_back(a.PreserveLuminosity ? 1.0f : 0.0f);
                                break;
                            case AdjustmentType::Vibrance: Params.insert(Params.end(), { a.Vibrance, a.VibranceSaturation }); break;
                            case AdjustmentType::BlackWhite:
                                Params.insert(Params.end(), a.Weights.begin(), a.Weights.end());
                                Params.insert(Params.end(), { a.Tint ? 1.0f : 0.0f, a.TintColor.r, a.TintColor.g, a.TintColor.b });
                                break;
                            case AdjustmentType::PhotoFilter:
                                Params.insert(Params.end(), { a.FilterColor.r, a.FilterColor.g, a.FilterColor.b, a.Density, a.PreserveLuminosity ? 1.0f : 0.0f });
                                break;
                            case AdjustmentType::Threshold: Params.push_back(a.ThresholdLevel); break;
                            case AdjustmentType::GradientMap: Params.insert(Params.end(), { a.MapFrom.r, a.MapFrom.g, a.MapFrom.b, a.MapTo.r, a.MapTo.g, a.MapTo.b }); break;
                            default: Params.push_back(0.0f); break;
                            }
                        while (Params.size() % 4) Params.push_back(0.0f);
                        ParamOps.push_back(Ops.size());
                        Ops.push_back(op);
                        continue;
                    }
                    if (!l.IsRaster() || !l.Pixels || l.Pixels->Width != Doc.Width || l.Pixels->Height != Doc.Height) continue;
                    // Effects under it, then its pixels (with the overlays on them when it has effects).
                    const std::shared_ptr<const StyledLayer>& style = l.Effects.Any() ? l.Styled : nullptr;
                    if (style)
                        for (const StyledLayer::Under& u : style->Below)
                        {
                            Op under;
                            under.Kind = 1u;
                            under.Mode = (uint32_t)u.Mode;
                            under.Opacity = u.Opacity * l.Opacity;
                            under.Pixels = Upload(std::shared_ptr<const void>(style, &u.Alpha), u.Alpha.data(), u.Alpha.size());
                            under.Color[0] = u.Color.r, under.Color[1] = u.Color.g, under.Color[2] = u.Color.b, under.Color[3] = 1.0f;
                            Ops.push_back(under);
                        }
                    Op op;
                    op.Kind = 0u;
                    op.Mode = (uint32_t)l.Blend;
                    op.Opacity = l.Opacity;
                    Mask(op, l);
                    const Bitmap& pixels = style ? style->Content : *l.Pixels;
                    std::shared_ptr<const void> hold = style ? std::shared_ptr<const void>(style, &style->Content) : std::shared_ptr<const void>(l.Pixels);
                    op.Pixels = Upload(hold, pixels.Pixels.data(), pixels.Pixels.size() * 4);
                    Ops.push_back(op);
                }
            }
        };
    }

    bool Available() { return RenderDevice::Exists(); }

    double LastMilliseconds() { return S().LastMs; }

    void Release()
    {
        State& s = S();
        s.Buffers.clear();
        s.Ops = Buffer();
        s.Params = Buffer();
        s.Out = Buffer();
        s.BlurA = Buffer();
        s.BlurB = Buffer();
        s.Composite = Pipeline();
        s.Blur = Pipeline();
        s.Ready = false;
    }

    void Touch(const void* data, Rect area, uint32_t width, uint32_t bpp)
    {
        if (!data || area.Empty()) return;
        auto it = S().Buffers.find(data);
        if (it == S().Buffers.end()) return;
        uint8_t* dst = (uint8_t*)it->second.Buf.GetMapped();
        const uint8_t* src = (const uint8_t*)data;
        const size_t limit = it->second.Buf.GetSize();
        for (int y = area.Y0; y <= area.Y1; ++y)
        {
            size_t offset = ((size_t)y * width + (size_t)area.X0) * bpp, bytes = (size_t)(area.X1 - area.X0 + 1) * bpp;
            if (offset + bytes <= limit) std::memcpy(dst + offset, src + offset, bytes);
        }
    }

    bool Composite(const Document& doc, Bitmap& out, Rect area)
    {
        if (!doc.Valid() || !Init()) return false;
        const auto start = std::chrono::steady_clock::now();
        doc.PrepareStyles();
        if (out.Width != doc.Width || out.Height != doc.Height || out.Pixels.size() != (size_t)doc.Width * doc.Height)
        {
            out = Bitmap::Solid(doc.Width, doc.Height, 0u);
            area = {};
        }
        if (area.Empty()) area = Rect{ 0, 0, (int)doc.Width - 1, (int)doc.Height - 1 };
        area.X0 = std::max(area.X0, 0), area.Y0 = std::max(area.Y0, 0);
        area.X1 = std::min(area.X1, (int)doc.Width - 1), area.Y1 = std::min(area.Y1, (int)doc.Height - 1);
        if (area.Empty()) return true;

        State& s = S();
        for (auto& [key, r] : s.Buffers) r.Used = false;
        Builder b{ doc, {}, {}, {}, {} };
        for (size_t i = 0; i < doc.Layers.size(); ++i) b.Kids[doc.Layers[i].Parent].push_back((int)i);
        b.Emit(0);
        if (b.MaxDepth >= 15) return false; // (the shader's stack is 16 deep)

        // The operations and the adjustments' settings up, the output's place ready.
        Ensure(s.Params, b.Params.size() * 4, MemoryUsage::CpuToGpu, "ImageParams");
        const VkDeviceAddress params = AddressOf(s.Params);
        for (size_t k : b.ParamOps) b.Ops[k].Aux = params + b.Ops[k].Aux * 4;
        if (!b.Params.empty()) std::memcpy(s.Params.GetMapped(), b.Params.data(), b.Params.size() * 4);
        Ensure(s.Ops, b.Ops.size() * sizeof(Op), MemoryUsage::CpuToGpu, "ImageOps");
        if (!b.Ops.empty()) std::memcpy(s.Ops.GetMapped(), b.Ops.data(), b.Ops.size() * sizeof(Op));
        const uint32_t aw = (uint32_t)(area.X1 - area.X0 + 1), ah = (uint32_t)(area.Y1 - area.Y0 + 1);
        Ensure(s.Out, (size_t)aw * ah * 4, MemoryUsage::GpuToCpu, "ImageOut");

        struct Push
        {
            uint64_t Ops, Out;
            uint32_t OpCount, Width, Height, X0, Y0, AreaW, AreaH;
        } push{ AddressOf(s.Ops), AddressOf(s.Out), (uint32_t)b.Ops.size(), doc.Width, doc.Height, (uint32_t)area.X0, (uint32_t)area.Y0, aw, ah };
        RenderDevice& device = RenderDevice::Get();
        device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            s.Composite.Bind(cmd);
            vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (aw + 15) / 16, (ah + 15) / 16, 1);
            Barrier(cmd, VK_ACCESS_2_HOST_READ_BIT, VK_PIPELINE_STAGE_2_HOST_BIT);
        });
        const uint32_t* result = (const uint32_t*)s.Out.GetMapped();
        for (uint32_t y = 0; y < ah; ++y)
            std::memcpy(out.Pixels.data() + (size_t)(area.Y0 + (int)y) * doc.Width + area.X0, result + (size_t)y * aw, (size_t)aw * 4);
        // What this composite did not use goes (old versions of layers, closed documents).
        for (auto it = s.Buffers.begin(); it != s.Buffers.end();)
            it = it->second.Used ? std::next(it) : s.Buffers.erase(it);
        s.LastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }

    bool BoxBlur(std::vector<glm::vec4>& colors, int w, int h, int radius, int passes)
    {
        if (w <= 0 || h <= 0 || !Init()) return false;
        State& s = S();
        const size_t bytes = colors.size() * sizeof(glm::vec4);
        Ensure(s.BlurA, bytes, MemoryUsage::GpuToCpu, "ImageBlurA");
        Ensure(s.BlurB, bytes, MemoryUsage::GpuToCpu, "ImageBlurB");
        std::memcpy(s.BlurA.GetMapped(), colors.data(), bytes);
        const VkDeviceAddress a = AddressOf(s.BlurA), bb = AddressOf(s.BlurB);
        struct Push
        {
            uint64_t In, Out;
            int32_t Width, Height, Radius, Horizontal;
        };
        RenderDevice& device = RenderDevice::Get();
        device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            s.Blur.Bind(cmd);
            // Rows then columns, `passes` times, back and forth between the two buffers (ending where it began).
            for (int k = 0; k < passes * 2; ++k)
            {
                Push push{ k % 2 == 0 ? a : bb, k % 2 == 0 ? bb : a, w, h, radius, k % 2 == 0 ? 1 : 0 };
                vkCmdPushConstants(cmd, device.GetPipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, (uint32_t)(w + 15) / 16, (uint32_t)(h + 15) / 16, 1);
                Barrier(cmd, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            }
            Barrier(cmd, VK_ACCESS_2_HOST_READ_BIT, VK_PIPELINE_STAGE_2_HOST_BIT);
        });
        std::memcpy(colors.data(), s.BlurA.GetMapped(), bytes);
        return true;
    }

    // ---- The check

    std::string SelfCheck(bool& ok)
    {
        std::ostringstream report;
        ok = true;
        if (!Available())
        {
            ok = false;
            return "No GPU (RenderDevice) to check";
        }
        uint32_t seed = 12345;
        auto rnd = [&] {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) / float(1 << 24);
        };
        auto noise = [&](uint32_t w, uint32_t h, bool alpha) {
            Bitmap b = Bitmap::Solid(w, h, 0u);
            // (blocks of color, so blends see a range of values; and some clear pixels)
            glm::vec4 c(0.0f);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    if ((x % 7) == 0 || (y % 5) == 0) c = glm::vec4(rnd(), rnd(), rnd(), alpha ? (rnd() < 0.15f ? 0.0f : rnd()) : 1.0f);
                    b.At(x, y) = Pack(c);
                }
            return b;
        };
        auto maskOf = [&](uint32_t w, uint32_t h) {
            Mask m = Mask::Solid(w, h, 0);
            for (size_t i = 0; i < m.Values.size(); ++i) m.Values[i] = (uint8_t)(rnd() * 255.0f);
            return std::make_shared<Mask>(m);
        };
        auto compare = [&](const char* what, const Bitmap& cpu, const Bitmap& gpu, Rect area) {
            int worst = 0;
            size_t off = 0, count = 0;
            if (area.Empty()) area = Rect{ 0, 0, (int)cpu.Width - 1, (int)cpu.Height - 1 };
            for (int y = area.Y0; y <= area.Y1; ++y)
                for (int x = area.X0; x <= area.X1; ++x)
                {
                    uint32_t a = cpu.At((uint32_t)x, (uint32_t)y), g = gpu.At((uint32_t)x, (uint32_t)y);
                    int d = 0;
                    // (clear pixels' colors do not matter)
                    if ((a >> 24) == 0 && (g >> 24) == 0) d = 0;
                    else
                        for (int s = 0; s < 32; s += 8) d = std::max(d, std::abs((int)((a >> s) & 0xFF) - (int)((g >> s) & 0xFF)));
                    worst = std::max(worst, d);
                    off += d > 2;
                    ++count;
                }
            // A few pixels may differ more: Dissolve's coin flip and blend edges land a rounding apart.
            bool pass = off <= count / 500;
            ok &= pass;
            report << (pass ? "PASS " : "FAIL ") << what << ": largest difference " << worst << "/255, " << off << " of " << count
                   << " pixels more than 2 apart\n";
        };

        // Every blend mode over a picture, masks and opacities, groups both ways, every adjustment, effects, text, shapes.
        const uint32_t W = 257, H = 131;
        Document doc = Document::Create(W, H, glm::vec4(0.5f, 0.6f, 0.7f, 1.0f));
        doc.Layers[0].Pixels = std::make_shared<Bitmap>(noise(W, H, false));
        for (int m = 0; m < (int)LayerBlend::PassThrough; ++m)
        {
            int i = doc.AddLayer(std::string("Mode ") + LayerBlendName((LayerBlend)m), (int)doc.Layers.size());
            doc.Layers[(size_t)i].Pixels = std::make_shared<Bitmap>(noise(W, H, true));
            doc.Layers[(size_t)i].Blend = (LayerBlend)m;
            doc.Layers[(size_t)i].Opacity = 0.3f + 0.7f * rnd();
            if (m % 3 == 0) doc.Layers[(size_t)i].LayerMask = maskOf(W, H);
        }
        int g1 = doc.GroupLayer((int)doc.Layers.size() - 1);
        doc.Layers[(size_t)g1].Blend = LayerBlend::Multiply;
        doc.Layers[(size_t)g1].Opacity = 0.8f;
        doc.Layers[(size_t)g1].LayerMask = maskOf(W, H);
        int g2 = doc.GroupLayer(doc.IndexOf(doc.Layers[(size_t)g1].Id));
        doc.Layers[(size_t)g2].Opacity = 0.6f; // (Pass Through, holding the Multiply group)
        doc.Layers[(size_t)g2].LayerMask = maskOf(W, H);
        for (int t = 0; t < (int)AdjustmentType::Count; ++t)
        {
            Adjustment a;
            a.Type = (AdjustmentType)t;
            a.Brightness = 20, a.Contrast = 30, a.InWhite[0] = 0.8f, a.Gamma[2] = 1.4f, a.Curve[1] = { { 0, 0.1f }, { 0.5f, 0.6f }, { 1, 0.9f } };
            a.Exposure = 0.5f, a.Hue = 40, a.Saturation = 25, a.Lightness = -10, a.Balance[1] = { 30, -20, 10 }, a.Vibrance = 40;
            a.Tint = t % 2 == 0, a.Levels = 5, a.ThresholdLevel = 0.45f, a.MapFrom = { 0.1f, 0, 0.3f }, a.MapTo = { 1, 0.9f, 0.5f };
            int i = doc.AddAdjustment(a, (int)doc.Layers.size());
            doc.Layers[(size_t)i].Opacity = 0.15f;
            doc.Layers[(size_t)i].LayerMask = maskOf(W, H);
            if (t == 3) doc.Layers[(size_t)i].Blend = LayerBlend::Overlay;
        }
        int fx = doc.AddLayer("Effects", (int)doc.Layers.size());
        Bitmap shape = Bitmap::Solid(W, H, 0u);
        for (uint32_t y = 40; y < 90; ++y)
            for (uint32_t x = 60; x < 140; ++x) shape.At(x, y) = Pack({ 0.9f, 0.4f, 0.1f, 1 });
        doc.Layers[(size_t)fx].Pixels = std::make_shared<Bitmap>(shape);
        LayerEffects& e = doc.Layers[(size_t)fx].Effects;
        e.DropShadow.Enabled = e.OuterGlow.Enabled = e.Stroke.Enabled = e.InnerGlow.Enabled = e.GradientOverlay.Enabled = true;
        e.GradientOverlay.Opacity = 0.5f;
        TextData text;
        text.Text = "GPU";
        text.Position = { 150, 100 };
        text.Size = 40;
        doc.AddText(text, (int)doc.Layers.size());
        ShapeData star;
        star.Kind = ShapeKind::Star;
        star.Center = { 210, 50 };
        star.Size = { 60, 60 };
        star.Stroke = true;
        doc.AddShape(star, (int)doc.Layers.size());

        Bitmap cpu = doc.Flatten(), gpu;
        if (!Composite(doc, gpu))
        {
            ok = false;
            return "The GPU composite did not run";
        }
        compare("full composite (27 modes, groups, masks, 13 adjustments, effects, text, shapes)", cpu, gpu, {});
        // A part of it (as painting asks for): the rest untouched.
        Bitmap part = Bitmap::Solid(W, H, Pack({ 1, 0, 1, 1 }));
        Rect area{ 30, 20, 120, 70 };
        Composite(doc, part, area);
        compare("a part of the canvas", cpu, part, area);
        ok &= part.At(5, 5) == Pack({ 1, 0, 1, 1 });
        // A layer painted in place, then Touched.
        Bitmap& painted = const_cast<Bitmap&>(*doc.Layers[1].Pixels);
        for (uint32_t y = 10; y < 20; ++y)
            for (uint32_t x = 10; x < 30; ++x) painted.At(x, y) = Pack({ 0, 1, 0, 1 });
        Touch(painted.Pixels.data(), Rect{ 10, 10, 29, 19 }, W, 4);
        Composite(doc, gpu);
        compare("after painting in place", doc.Flatten(), gpu, {});

        // The blur.
        Bitmap pic = noise(300, 280, true);
        SetBoxBlurHook(nullptr);
        Bitmap cpuBlur = GaussianBlur(pic, 6.0f);
        SetBoxBlurHook(&BoxBlur);
        Bitmap gpuBlur = GaussianBlur(pic, 6.0f);
        SetBoxBlurHook(nullptr);
        compare("Gaussian blur", cpuBlur, gpuBlur, {});

        // How fast: a 2048 x 2048 picture of 8 layers and two adjustments.
        Document big = Document::Create(2048, 2048, glm::vec4(1.0f));
        for (int k = 0; k < 8; ++k)
        {
            int i = big.AddLayer("L", (int)big.Layers.size());
            big.Layers[(size_t)i].Pixels = std::make_shared<Bitmap>(noise(2048, 2048, true));
            big.Layers[(size_t)i].Blend = (LayerBlend)(k * 3 % 26);
        }
        Adjustment curves;
        curves.Type = AdjustmentType::Curves;
        big.AddAdjustment(curves, (int)big.Layers.size());
        Adjustment hue;
        hue.Type = AdjustmentType::HueSaturation;
        hue.Hue = 30;
        big.AddAdjustment(hue, (int)big.Layers.size());
        auto time = [](auto&& f) {
            auto t0 = std::chrono::steady_clock::now();
            f();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
        Bitmap bigCpu, bigGpu;
        double cpuMs = time([&] { big.Composite(bigCpu); });
        Composite(big, bigGpu); // (the first: layers go up)
        double gpuMs = time([&] { Composite(big, bigGpu); });
        double gpuPartMs = time([&] { Composite(big, bigGpu, Rect{ 900, 900, 1155, 1155 }); });
        compare("2048 x 2048, 10 layers", bigCpu, bigGpu, {});
        double blurCpu = time([&] {
            SetBoxBlurHook(nullptr);
            Bitmap tmp = GaussianBlur(Bitmap::Solid(2048, 2048, Pack(glm::vec4(0.5f))), 20.0f);
        });
        double blurGpu = time([&] {
            SetBoxBlurHook(&BoxBlur);
            Bitmap tmp = GaussianBlur(Bitmap::Solid(2048, 2048, Pack(glm::vec4(0.5f))), 20.0f);
            SetBoxBlurHook(nullptr);
        });
        char line[256];
        std::snprintf(line, sizeof(line), "TIME 2048x2048 composite: CPU %.1f ms, GPU %.1f ms (a 256 px part: %.1f ms); blur 20 px: CPU %.1f ms, GPU %.1f ms\n",
                      cpuMs, gpuMs, gpuPartMs, blurCpu, blurGpu);
        report << line;
        Release();
        return report.str();
    }
}
