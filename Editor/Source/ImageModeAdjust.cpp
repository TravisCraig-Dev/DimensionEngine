// Image Mode: adjustments (Image > Adjustments, and adjustment layers with their Properties), filters (the Filter menu)
// and layer styles, each shown live while its window is open.
#include "ImageMode.h"
#include "EditorUI.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Imaging;

    namespace
    {
        bool ColorEdit(const char* label, glm::vec3& c)
        {
            return ImGui::ColorEdit3(label, &c.r, ImGuiColorEditFlags_NoInputs);
        }
        bool ModeCombo(const char* label, LayerBlend& mode)
        {
            bool changed = false;
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo(label, LayerBlendName(mode)))
            {
                for (int m = 0; m < (int)LayerBlend::PassThrough; ++m)
                {
                    if (m > 0 && LayerBlendStartsGroup((LayerBlend)m)) ImGui::Separator();
                    if (ImGui::Selectable(LayerBlendName((LayerBlend)m), mode == (LayerBlend)m)) mode = (LayerBlend)m, changed = true;
                }
                ImGui::EndCombo();
            }
            return changed;
        }
    }

    // ---- Opening and finishing

    void ImageMode::OpenAdjustment(AdjustmentType type)
    {
        CommitTransform();
        EndPaint();
        FinishPreview(true);
        if (m_Doc.Layers.empty()) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        const bool mask = m_EditMask && l.HasMask();
        if (!mask && !l.HasPixels())
        {
            Note("Select a pixel layer (or a mask) to adjust, or use Layer > New Adjustment Layer");
            return;
        }
        if (!mask && (l.LockPixels || l.LockAll))
        {
            Note("The layer is locked");
            return;
        }
        m_Preview = PreviewState();
        m_Preview.Kind = 1;
        m_Preview.Layer = l.Id;
        m_Preview.Mask = mask;
        m_Preview.Pixels = l.Pixels;
        m_Preview.LayerMask = l.LayerMask;
        m_Preview.Adjust.Type = type;
        if (type == AdjustmentType::HueSaturation && ImGui::GetIO().KeyShift) m_Preview.Adjust.Saturation = -100.0f; // (Desaturate)
        m_Preview.Dirty = true;
        Record(AdjustmentName(type));
        // Nothing to set: done at once.
        if (type == AdjustmentType::Invert) FinishPreview(true);
    }

    void ImageMode::OpenFilter(FilterType type)
    {
        CommitTransform();
        EndPaint();
        FinishPreview(true);
        if (m_Doc.Layers.empty()) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        const bool mask = m_EditMask && l.HasMask();
        if (!mask && !l.HasPixels())
        {
            Note("Select a pixel layer (or a mask) to filter");
            return;
        }
        if (!mask && (l.LockPixels || l.LockAll))
        {
            Note("The layer is locked");
            return;
        }
        m_Preview = PreviewState();
        m_Preview.Kind = 2;
        m_Preview.Layer = l.Id;
        m_Preview.Mask = mask;
        m_Preview.Pixels = l.Pixels;
        m_Preview.LayerMask = l.LayerMask;
        FilterSettings& f = m_Preview.Filter;
        f.Type = type;
        switch (type)
        {
        case FilterType::GaussianBlur:
        case FilterType::BoxBlur: f.Radius = 4.0f; break;
        case FilterType::MotionBlur: f.Angle = 0.0f, f.Distance = 20.0f; break;
        case FilterType::UnsharpMask: f.Amount = 1.0f, f.Radius = 2.0f, f.Threshold = 0.0f; break;
        case FilterType::AddNoise: f.Amount = 0.5f; break;
        case FilterType::Median: f.Radius = 2.0f; break;
        case FilterType::HighPass: f.Radius = 10.0f; break;
        case FilterType::Mosaic: f.Radius = 8.0f; break;
        case FilterType::Emboss: f.Angle = 135.0f, f.Distance = 3.0f, f.Amount = 1.0f; break;
        case FilterType::Offset: f.Shift = glm::ivec2((int)m_Doc.Width / 2, (int)m_Doc.Height / 2); break;
        case FilterType::MakeSeamless: f.Amount = 0.5f; break;
        case FilterType::NormalMap: f.Amount = 2.0f, f.Wrap = true; break;
        case FilterType::ReduceColors:
            if (!m_PalettesLoaded) LoadPalettes();
            if (m_ReduceUseSwatches) f.Palette = m_Palettes[(size_t)m_PaletteIndex].Colors;
            f.Colors = 16;
            break;
        default: break;
        }
        m_Preview.Dirty = true;
        Record(FilterName(type));
        if (type == FilterType::Sharpen || type == FilterType::FindEdges) FinishPreview(true);
    }

    void ImageMode::OpenLayerStyle(int section)
    {
        CommitTransform();
        EndPaint();
        FinishPreview(true);
        if (m_Doc.Layers.empty()) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (!l.IsRaster())
        {
            Note("Layer styles go on pixel, text and shape layers");
            return;
        }
        m_Preview = PreviewState();
        m_Preview.Kind = 3;
        m_Preview.Layer = l.Id;
        m_Preview.Effects = m_Preview.OriginalEffects = l.Effects;
        m_Preview.Section = section;
        Record("Layer Style");
    }

    void ImageMode::NewAdjustmentLayer(AdjustmentType type)
    {
        CommitTransform();
        FinishPreview(true);
        Adjustment a;
        a.Type = type;
        Record(std::string("New ") + AdjustmentName(type) + " Layer");
        uint32_t parent = m_Doc.Layers.empty() ? 0 : m_Doc.Layers[(size_t)m_ActiveLayer].Parent;
        int index = m_Doc.AddAdjustment(a, m_ActiveLayer + 1, parent);
        // With a selection: it works only there (its mask), as Photoshop's do.
        if (m_Doc.Selection)
        {
            m_Doc.Layers[(size_t)index].LayerMask = m_Doc.Selection;
            m_LastSelection = m_Doc.Selection;
            m_Doc.Selection.reset();
        }
        SetActive(index);
        m_ShowProperties = true;
        Changed();
        MarkAll();
    }

    void ImageMode::UpdatePreview()
    {
        if (!m_Preview.Kind || !m_Preview.Dirty) return;
        m_Preview.Dirty = false;
        int i = m_Doc.IndexOf(m_Preview.Layer);
        if (i < 0)
        {
            m_Preview = PreviewState();
            return;
        }
        Layer& l = m_Doc.Layers[(size_t)i];
        if (m_Preview.Kind == 3)
            l.Effects = m_Preview.Show ? m_Preview.Effects : m_Preview.OriginalEffects;
        else if (!m_Preview.Show)
        {
            l.Pixels = m_Preview.Pixels;
            l.LayerMask = m_Preview.LayerMask;
        }
        else
        {
            const Mask* sel = m_Doc.Selection.get();
            auto change = [&](const Bitmap& b) {
                return m_Preview.Kind == 1 ? ApplyAdjustment(b, m_Preview.Adjust, sel) : ApplyFilter(b, m_Preview.Filter, sel);
            };
            if (m_Preview.Mask && m_Preview.LayerMask) l.LayerMask = std::make_shared<Mask>(ApplyToMask(*m_Preview.LayerMask, change));
            else if (m_Preview.Pixels)
            {
                Bitmap out = change(*m_Preview.Pixels);
                if (l.LockTransparency)
                    for (size_t k = 0; k < out.Pixels.size(); ++k) out.Pixels[k] = (out.Pixels[k] & 0x00FFFFFFu) | (m_Preview.Pixels->Pixels[k] & 0xFF000000u);
                l.Pixels = std::make_shared<Bitmap>(std::move(out));
            }
        }
        MarkAll();
    }

    void ImageMode::FinishPreview(bool keep)
    {
        if (!m_Preview.Kind) return;
        if (keep)
        {
            if (!m_Preview.Show) m_Preview.Show = true, m_Preview.Dirty = true;
            UpdatePreview();
            if (m_Preview.Kind == 3 && m_Preview.Effects.Hash() == m_Preview.OriginalEffects.Hash() && !m_Undo.empty()) m_Undo.pop_back();
            else Changed();
        }
        else
        {
            if (int i = m_Doc.IndexOf(m_Preview.Layer); i >= 0)
            {
                Layer& l = m_Doc.Layers[(size_t)i];
                if (m_Preview.Kind == 3) l.Effects = m_Preview.OriginalEffects;
                else
                {
                    l.Pixels = m_Preview.Pixels;
                    l.LayerMask = m_Preview.LayerMask;
                }
            }
            if (!m_Undo.empty()) m_Undo.pop_back();
            MarkAll();
        }
        m_Preview = PreviewState();
    }

    // ---- Windows

    void ImageMode::DrawPreviewWindow()
    {
        if (!m_Preview.Kind) return;
        const char* title = m_Preview.Kind == 1 ? AdjustmentName(m_Preview.Adjust.Type) : m_Preview.Kind == 2 ? FilterName(m_Preview.Filter.Type) : "Layer Style";
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 380.0f, vp->WorkPos.y + 120.0f), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
        bool open = true;
        std::string id = std::string(title) + "##imagePreview";
        if (ImGui::Begin(id.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
        {
            bool started = false;
            if (m_Preview.Kind == 1) m_Preview.Dirty |= DrawAdjustmentUI(m_Preview.Adjust, started);
            else if (m_Preview.Kind == 2) m_Preview.Dirty |= DrawFilterUI(m_Preview.Filter, started);
            else m_Preview.Dirty |= DrawEffectsUI(m_Preview.Effects, m_Preview.Section);
            ImGui::Separator();
            if (ImGui::Checkbox("Preview", &m_Preview.Show)) m_Preview.Dirty = true;
            ImGui::SameLine(0.0f, 24.0f);
            if (ImGui::Button("OK", ImVec2(90, 0))) FinishPreview(true);
            else
            {
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(90, 0))) FinishPreview(false);
            }
        }
        ImGui::End();
        if (!open) FinishPreview(false);
    }

    void ImageMode::DrawProperties()
    {
        if (m_Doc.Layers.empty()) return;
        Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (l.IsAdjustment())
        {
            ImGui::TextColored(UI::AccentText(), "%s", AdjustmentName(l.Adjust.Type));
            ImGui::Separator();
            // Changes go to a copy first, so the undo step taken as an edit begins holds the settings from before it.
            Adjustment copy = l.Adjust;
            bool started = false;
            bool changed = DrawAdjustmentUI(copy, started);
            if ((started || changed) && !m_PropsRecorded)
            {
                Record(std::string(AdjustmentName(l.Adjust.Type)) + " Settings");
                m_PropsRecorded = true;
            }
            if (changed)
            {
                m_Doc.Layers[(size_t)m_ActiveLayer].Adjust = copy;
                Changed();
                MarkAll();
            }
            if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_PropsRecorded = false;
            ImGui::Spacing();
            ImGui::TextDisabled("Its mask limits where it works; paint it in gray.");
        }
        else if (l.IsVector())
            DrawVectorProperties();
        else if (l.HasPixels())
        {
            ImGui::TextColored(UI::AccentText(), "%s", l.Name.c_str());
            ImGui::Separator();
            if (ImGui::Button("Layer Style...")) OpenLayerStyle();
            ImGui::SameLine();
            ImGui::BeginDisabled(!l.Effects.Any());
            if (ImGui::Button("Clear Layer Style"))
            {
                Record("Clear Layer Style");
                m_Doc.Layers[(size_t)m_ActiveLayer].Effects = LayerEffects();
                Changed();
                MarkAll();
            }
            ImGui::EndDisabled();
            // The effects on: on and off quickly.
            struct Item
            {
                const char* Name;
                bool* On;
                int Section;
            };
            LayerEffects& e = m_Doc.Layers[(size_t)m_ActiveLayer].Effects;
            Item items[] = { { "Drop Shadow", &e.DropShadow.Enabled, 0 },     { "Inner Shadow", &e.InnerShadow.Enabled, 1 },
                             { "Outer Glow", &e.OuterGlow.Enabled, 2 },       { "Inner Glow", &e.InnerGlow.Enabled, 3 },
                             { "Stroke", &e.Stroke.Enabled, 4 },              { "Color Overlay", &e.ColorOverlay.Enabled, 5 },
                             { "Gradient Overlay", &e.GradientOverlay.Enabled, 6 } };
            ImGui::Spacing();
            for (const Item& it : items)
            {
                bool on = *it.On;
                ImGui::PushID(it.Section);
                if (ImGui::Checkbox("##on", &on))
                {
                    Record(on ? "Effect On" : "Effect Off"); // (the snapshot copies the layers; `it.On` stays this layer's)
                    *it.On = on;
                    Changed();
                    MarkAll();
                }
                ImGui::SameLine();
                if (ImGui::Selectable(it.Name)) OpenLayerStyle(it.Section);
                ImGui::PopID();
            }
        }
        else
            ImGui::TextDisabled("A group: select a layer in it, or an adjustment layer, to see its settings.");
    }
}

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Imaging;

    // ---- Menus

    void ImageMode::DrawAdjustmentMenu(bool asLayer)
    {
        static const char* keys[(int)AdjustmentType::Count] = { nullptr, "Ctrl+L", "Ctrl+M", nullptr, "Ctrl+U", "Ctrl+B", nullptr, nullptr, nullptr, "Ctrl+I" };
        for (int t = 0; t < (int)AdjustmentType::Count; ++t)
        {
            if (t == (int)AdjustmentType::Exposure || t == (int)AdjustmentType::Vibrance || t == (int)AdjustmentType::Invert) ImGui::Separator();
            if (ImGui::MenuItem(AdjustmentName((AdjustmentType)t), asLayer ? nullptr : keys[t]))
            {
                if (asLayer) NewAdjustmentLayer((AdjustmentType)t);
                else OpenAdjustment((AdjustmentType)t);
            }
        }
        if (!asLayer)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Desaturate", "Ctrl+Shift+U"))
            {
                OpenAdjustment(AdjustmentType::HueSaturation);
                if (m_Preview.Kind == 1)
                {
                    m_Preview.Adjust.Saturation = -100.0f;
                    m_Preview.Dirty = true;
                    FinishPreview(true);
                }
            }
        }
    }

    void ImageMode::DrawFilterMenu()
    {
        auto item = [&](FilterType t) {
            if (ImGui::MenuItem(FilterName(t))) OpenFilter(t);
        };
        if (ImGui::BeginMenu("Blur"))
        {
            item(FilterType::GaussianBlur);
            item(FilterType::BoxBlur);
            item(FilterType::MotionBlur);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Sharpen"))
        {
            item(FilterType::Sharpen);
            item(FilterType::UnsharpMask);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Noise"))
        {
            item(FilterType::AddNoise);
            item(FilterType::Median);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Stylize"))
        {
            item(FilterType::Emboss);
            item(FilterType::FindEdges);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Pixelate"))
        {
            item(FilterType::Mosaic);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Other"))
        {
            item(FilterType::HighPass);
            item(FilterType::Offset);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Game"))
        {
            item(FilterType::NormalMap);
            item(FilterType::MakeSeamless);
            item(FilterType::Offset);
            item(FilterType::ReduceColors);
            ImGui::EndMenu();
        }
    }

    // ---- Settings

    void ImageMode::RefreshHistogram(int channel)
    {
        if (channel == m_HistogramChannel && ++m_HistogramAge < 20) return;
        m_HistogramChannel = channel;
        m_HistogramAge = 0;
        m_Histogram = Histogram(m_Composite, channel);
    }

    bool ImageMode::CurveEditor(std::vector<glm::vec2>& pts, bool& started)
    {
        const float size = 256.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##curve", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto screen = [&](glm::vec2 v) { return ImVec2(pos.x + v.x * size, pos.y + (1.0f - v.y) * size); };
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const glm::vec2 m(std::clamp((mouse.x - pos.x) / size, 0.0f, 1.0f), (pos.y + size - mouse.y) / size);
        dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), IM_COL32(30, 30, 32, 255));
        // The histogram, faint, and quarter lines.
        uint32_t peak = 1;
        for (uint32_t c : m_Histogram) peak = std::max(peak, c);
        for (int i = 0; i < 256; ++i)
        {
            float hgt = std::sqrt(m_Histogram[(size_t)i] / (float)peak) * size;
            dl->AddLine(ImVec2(pos.x + i, pos.y + size), ImVec2(pos.x + i, pos.y + size - hgt), IM_COL32(90, 90, 95, 255));
        }
        for (int k = 1; k < 4; ++k)
        {
            dl->AddLine(ImVec2(pos.x + size * k / 4, pos.y), ImVec2(pos.x + size * k / 4, pos.y + size), IM_COL32(70, 70, 70, 255));
            dl->AddLine(ImVec2(pos.x, pos.y + size * k / 4), ImVec2(pos.x + size, pos.y + size * k / 4), IM_COL32(70, 70, 70, 255));
        }
        dl->AddLine(screen({ 0, 0 }), screen({ 1, 1 }), IM_COL32(110, 110, 110, 255));
        ImVec2 line[129];
        for (int i = 0; i <= 128; ++i)
        {
            float x = i / 128.0f;
            line[i] = screen({ x, CurveAt(pts, x) });
        }
        dl->AddPolyline(line, 129, IM_COL32(240, 240, 240, 255), 0, 1.5f);
        int near = -1;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            ImVec2 p = screen(pts[i]);
            if (std::abs(p.x - mouse.x) < 7.0f && std::abs(p.y - mouse.y) < 7.0f) near = (int)i;
        }
        for (size_t i = 0; i < pts.size(); ++i)
        {
            ImVec2 p = screen(pts[i]);
            bool on = (int)i == m_CurveDrag || (int)i == near;
            dl->AddRectFilled(ImVec2(p.x - 3.5f, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y + 3.5f), on ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255));
            dl->AddRect(ImVec2(p.x - 3.5f, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y + 3.5f), IM_COL32(240, 240, 240, 255));
        }
        bool changed = false;
        // Right-click a point: it goes (two are kept).
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && near >= 0 && pts.size() > 2)
        {
            started = true;
            pts.erase(pts.begin() + near);
            return true;
        }
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            started = true;
            if (near >= 0) m_CurveDrag = near;
            else
            {
                // A new point on the curve where clicked.
                glm::vec2 v(m.x, std::clamp(m.y, 0.0f, 1.0f));
                auto it = std::lower_bound(pts.begin(), pts.end(), v.x, [](const glm::vec2& p, float x) { return p.x < x; });
                m_CurveDrag = (int)(it - pts.begin());
                pts.insert(it, v);
                changed = true;
            }
        }
        if (active && m_CurveDrag >= 0 && m_CurveDrag < (int)pts.size())
        {
            // Between its neighbors; dragged well out of the box (with more than two), it goes.
            if ((m.y < -0.15f || m.y > 1.15f) && pts.size() > 2)
            {
                pts.erase(pts.begin() + m_CurveDrag);
                m_CurveDrag = -1;
                return true;
            }
            float lo = m_CurveDrag > 0 ? pts[(size_t)m_CurveDrag - 1].x + 0.01f : 0.0f;
            float hi = m_CurveDrag + 1 < (int)pts.size() ? pts[(size_t)m_CurveDrag + 1].x - 0.01f : 1.0f;
            glm::vec2 v(std::clamp(m.x, lo, std::max(lo, hi)), std::clamp(m.y, 0.0f, 1.0f));
            if (v != pts[(size_t)m_CurveDrag])
            {
                pts[(size_t)m_CurveDrag] = v;
                changed = true;
            }
        }
        if (!active) m_CurveDrag = -1;
        if (hovered && near >= 0) ImGui::SetTooltip("%.0f -> %.0f", pts[(size_t)near].x * 255.0f, pts[(size_t)near].y * 255.0f);
        return changed;
    }

    bool ImageMode::DrawAdjustmentUI(Adjustment& a, bool& started)
    {
        bool changed = false;
        // Drags count as one edit from when they begin; switches and pickers as they change.
        auto drag = [&](bool c) {
            if (ImGui::IsItemActivated()) started = true;
            changed |= c;
        };
        auto pick = [&](bool c) {
            if (c) started = true, changed = true;
        };
        auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt) {
            ImGui::SetNextItemWidth(220.0f);
            drag(ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp));
        };
        // 0..1 values shown 0..255.
        auto slider255 = [&](const char* label, float* v) {
            float x = *v * 255.0f;
            ImGui::SetNextItemWidth(220.0f);
            bool c = ImGui::SliderFloat(label, &x, 0.0f, 255.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            drag(c);
            if (c) *v = x / 255.0f;
        };
        static const char* channels[] = { "RGB", "Red", "Green", "Blue" };
        switch (a.Type)
        {
        case AdjustmentType::BrightnessContrast:
            slider("Brightness", &a.Brightness, -100.0f, 100.0f, "%.0f");
            slider("Contrast", &a.Contrast, -100.0f, 100.0f, "%.0f");
            break;
        case AdjustmentType::Levels:
        {
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Channel##levels", &m_LevelsChannel, channels, 4);
            const size_t ch = (size_t)m_LevelsChannel;
            RefreshHistogram(m_LevelsChannel);
            // The histogram with the input points under it.
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(256, 90));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + 256, pos.y + 90), IM_COL32(30, 30, 32, 255));
            uint32_t peak = 1;
            for (uint32_t c : m_Histogram) peak = std::max(peak, c);
            for (int i = 0; i < 256; ++i)
                dl->AddLine(ImVec2(pos.x + i, pos.y + 90), ImVec2(pos.x + i, pos.y + 90 - std::sqrt(m_Histogram[(size_t)i] / (float)peak) * 88.0f), IM_COL32(170, 170, 175, 255));
            auto marker = [&](float v, ImU32 col) {
                float x = pos.x + v * 255.0f;
                dl->AddTriangleFilled(ImVec2(x, pos.y + 84), ImVec2(x - 5, pos.y + 94), ImVec2(x + 5, pos.y + 94), col);
            };
            float mid = a.InBlack[ch] + (a.InWhite[ch] - a.InBlack[ch]) * std::pow(0.5f, a.Gamma[ch]);
            marker(a.InBlack[ch], IM_COL32(0, 0, 0, 255));
            marker(mid, IM_COL32(128, 128, 128, 255));
            marker(a.InWhite[ch], IM_COL32(255, 255, 255, 255));
            ImGui::Dummy(ImVec2(0, 4));
            slider255("Input Black", &a.InBlack[ch]);
            slider("Midtones", &a.Gamma[ch], 0.1f, 9.99f, "%.2f");
            slider255("Input White", &a.InWhite[ch]);
            slider255("Output Black", &a.OutBlack[ch]);
            slider255("Output White", &a.OutWhite[ch]);
            a.InWhite[ch] = std::max(a.InWhite[ch], a.InBlack[ch] + 2.0f / 255.0f);
            break;
        }
        case AdjustmentType::Curves:
        {
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Channel##curves", &m_CurvesChannel, channels, 4);
            RefreshHistogram(m_CurvesChannel);
            std::vector<glm::vec2>& pts = a.Curve[(size_t)m_CurvesChannel];
            changed |= CurveEditor(pts, started);
            if (ImGui::Button("Reset Channel")) pick(true), pts = { { 0, 0 }, { 1, 1 } };
            ImGui::SameLine();
            ImGui::TextDisabled("Click to add a point; drag it; right-click to remove");
            break;
        }
        case AdjustmentType::Exposure:
            slider("Exposure", &a.Exposure, -5.0f, 5.0f, "%.2f stops");
            slider("Offset", &a.Offset, -0.5f, 0.5f, "%.4f");
            slider("Gamma", &a.ExposureGamma, 0.1f, 9.99f, "%.2f");
            break;
        case AdjustmentType::HueSaturation:
            pick(ImGui::Checkbox("Colorize", &a.Colorize));
            if (a.Colorize)
            {
                a.Hue = std::clamp(a.Hue < 0.0f ? a.Hue + 360.0f : a.Hue, 0.0f, 360.0f);
                a.Saturation = std::clamp(a.Saturation, 0.0f, 100.0f);
                slider("Hue", &a.Hue, 0.0f, 360.0f, "%.0f");
                slider("Saturation", &a.Saturation, 0.0f, 100.0f, "%.0f");
            }
            else
            {
                slider("Hue", &a.Hue, -180.0f, 180.0f, "%.0f");
                slider("Saturation", &a.Saturation, -100.0f, 100.0f, "%.0f");
            }
            slider("Lightness", &a.Lightness, -100.0f, 100.0f, "%.0f");
            break;
        case AdjustmentType::ColorBalance:
        {
            static const char* tones[] = { "Shadows", "Midtones", "Highlights" };
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Tone", &m_BalanceTone, tones, 3);
            glm::vec3& b = a.Balance[(size_t)m_BalanceTone];
            slider("Cyan - Red", &b.x, -100.0f, 100.0f, "%.0f");
            slider("Magenta - Green", &b.y, -100.0f, 100.0f, "%.0f");
            slider("Yellow - Blue", &b.z, -100.0f, 100.0f, "%.0f");
            pick(ImGui::Checkbox("Preserve Luminosity", &a.PreserveLuminosity));
            break;
        }
        case AdjustmentType::Vibrance:
            slider("Vibrance", &a.Vibrance, -100.0f, 100.0f, "%.0f");
            slider("Saturation", &a.VibranceSaturation, -100.0f, 100.0f, "%.0f");
            break;
        case AdjustmentType::BlackWhite:
        {
            static const char* names[] = { "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas" };
            for (int i = 0; i < 6; ++i) slider(names[i], &a.Weights[(size_t)i], -200.0f, 300.0f, "%.0f%%");
            pick(ImGui::Checkbox("Tint", &a.Tint));
            if (a.Tint)
            {
                ImGui::SameLine();
                pick(ColorEdit("##tint", a.TintColor));
            }
            break;
        }
        case AdjustmentType::PhotoFilter:
            pick(ColorEdit("Filter Color", a.FilterColor));
            {
                float d = a.Density * 100.0f;
                ImGui::SetNextItemWidth(220.0f);
                bool c = ImGui::SliderFloat("Density", &d, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                drag(c);
                if (c) a.Density = d / 100.0f;
            }
            pick(ImGui::Checkbox("Preserve Luminosity", &a.PreserveLuminosity));
            break;
        case AdjustmentType::Invert: ImGui::TextDisabled("Every color turned to its opposite."); break;
        case AdjustmentType::Posterize:
            ImGui::SetNextItemWidth(220.0f);
            drag(ImGui::SliderInt("Levels", &a.Levels, 2, 255, "%d", ImGuiSliderFlags_AlwaysClamp));
            break;
        case AdjustmentType::Threshold: slider255("Threshold Level", &a.ThresholdLevel); break;
        case AdjustmentType::GradientMap:
            pick(ColorEdit("Shadows", a.MapFrom));
            ImGui::SameLine();
            pick(ColorEdit("Highlights", a.MapTo));
            if (ImGui::Button("From the Colors")) pick(true), a.MapFrom = glm::vec3(m_Foreground), a.MapTo = glm::vec3(m_Background);
            ImGui::SetItemTooltip("The foreground color for the shadows, the background color for the highlights");
            break;
        default: break;
        }
        return changed;
    }

    bool ImageMode::DrawFilterUI(FilterSettings& f, bool& started)
    {
        bool changed = false;
        auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt, bool log = false) {
            ImGui::SetNextItemWidth(220.0f);
            bool c = ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
            if (ImGui::IsItemActivated()) started = true;
            changed |= c;
        };
        switch (f.Type)
        {
        case FilterType::GaussianBlur:
        case FilterType::BoxBlur: slider("Radius", &f.Radius, 0.1f, 250.0f, "%.1f px", true); break;
        case FilterType::MotionBlur:
            slider("Angle", &f.Angle, -180.0f, 180.0f, "%.0f deg");
            slider("Distance", &f.Distance, 1.0f, 500.0f, "%.0f px", true);
            break;
        case FilterType::UnsharpMask:
        {
            float amount = f.Amount * 100.0f;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Amount", &amount, 1.0f, 500.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) f.Amount = amount / 100.0f, changed = true;
            slider("Radius", &f.Radius, 0.1f, 250.0f, "%.1f px", true);
            slider("Threshold", &f.Threshold, 0.0f, 255.0f, "%.0f levels");
            break;
        }
        case FilterType::AddNoise:
        {
            float amount = f.Amount * 100.0f;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Amount", &amount, 0.1f, 400.0f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp)) f.Amount = amount / 100.0f, changed = true;
            int dist = f.Gaussian ? 1 : 0;
            static const char* dists[] = { "Uniform", "Gaussian" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Distribution", &dist, dists, 2)) f.Gaussian = dist == 1, changed = true;
            changed |= ImGui::Checkbox("Monochromatic", &f.Monochrome);
            int seed = (int)f.Seed;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputInt("Seed", &seed)) f.Seed = (uint32_t)std::max(seed, 0), changed = true;
            break;
        }
        case FilterType::Median: slider("Radius", &f.Radius, 1.0f, 50.0f, "%.0f px"); break;
        case FilterType::HighPass: slider("Radius", &f.Radius, 0.1f, 250.0f, "%.1f px", true); break;
        case FilterType::Mosaic: slider("Cell Size", &f.Radius, 2.0f, 200.0f, "%.0f px", true); break;
        case FilterType::Emboss:
        {
            slider("Angle", &f.Angle, -180.0f, 180.0f, "%.0f deg");
            slider("Height", &f.Distance, 1.0f, 10.0f, "%.0f px");
            float amount = f.Amount * 100.0f;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Amount", &amount, 1.0f, 500.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) f.Amount = amount / 100.0f, changed = true;
            break;
        }
        case FilterType::Offset:
        {
            ImGui::SetNextItemWidth(220.0f);
            changed |= ImGui::DragInt2("Right, Down", &f.Shift.x, 1.0f, -(int)m_Doc.Width, (int)m_Doc.Width, "%d px");
            if (ImGui::Button("Half the Size")) f.Shift = glm::ivec2((int)m_Doc.Width / 2, (int)m_Doc.Height / 2), changed = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(the edges wrap: seams show in the middle)");
            break;
        }
        case FilterType::MakeSeamless:
        {
            float blend = f.Amount * 100.0f;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Blend", &blend, 5.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) f.Amount = blend / 100.0f, changed = true;
            ImGui::TextDisabled("Tiles without seams (the View's Tile shows it repeated).");
            break;
        }
        case FilterType::NormalMap:
            slider("Strength", &f.Amount, 0.1f, 30.0f, "%.1f", true);
            changed |= ImGui::Checkbox("Invert Y (DirectX)", &f.InvertY);
            ImGui::SetItemTooltip("Green pointing down; off: up, as OpenGL and this engine read them");
            changed |= ImGui::Checkbox("Wrap edges (tiling)", &f.Wrap);
            ImGui::TextDisabled("The picture's brightness is the height. Tip: Export As a _normal.png.");
            break;
        case FilterType::ReduceColors:
        {
            if (!m_PalettesLoaded) LoadPalettes();
            int source = f.Palette.empty() ? 1 : 0;
            std::string swatches = "The Swatches: " + m_Palettes[(size_t)m_PaletteIndex].Name;
            const char* sources[] = { swatches.c_str(), "Its own colors" };
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("Palette", &source, sources, 2))
            {
                f.Palette = source == 0 ? m_Palettes[(size_t)m_PaletteIndex].Colors : std::vector<glm::vec3>();
                m_ReduceUseSwatches = source == 0;
                changed = true;
            }
            if (source == 1)
            {
                ImGui::SetNextItemWidth(220.0f);
                changed |= ImGui::SliderInt("Colors", &f.Colors, 2, 256, "%d", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            }
            int dither = f.DitherKind;
            static const char* dithers[] = { "None", "Floyd-Steinberg (diffusion)", "Ordered (Bayer pattern)" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Dither", &dither, dithers, 3)) f.DitherKind = dither, changed = true;
            changed |= ImGui::Checkbox("Hard alpha (clear or solid)", &f.HardAlpha);
            break;
        }
        default: ImGui::TextDisabled("No settings."); break;
        }
        return changed;
    }

    bool ImageMode::DrawEffectsUI(LayerEffects& e, int& section)
    {
        bool changed = false;
        static const char* names[] = { "Drop Shadow", "Inner Shadow", "Outer Glow", "Inner Glow", "Stroke", "Color Overlay", "Gradient Overlay" };
        bool* on[] = { &e.DropShadow.Enabled, &e.InnerShadow.Enabled, &e.OuterGlow.Enabled, &e.InnerGlow.Enabled,
                       &e.Stroke.Enabled,     &e.ColorOverlay.Enabled, &e.GradientOverlay.Enabled };
        ImGui::BeginChild("##fxList", ImVec2(170, 300), ImGuiChildFlags_Borders);
        for (int i = 0; i < 7; ++i)
        {
            ImGui::PushID(i);
            changed |= ImGui::Checkbox("##on", on[i]);
            ImGui::SameLine();
            if (ImGui::Selectable(names[i], section == i)) section = i;
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##fxSettings", ImVec2(340, 300), ImGuiChildFlags_Borders);
        ImGui::TextColored(UI::AccentText(), "%s", names[section]);
        ImGui::Separator();
        auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt) {
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
        };
        auto percent = [&](const char* label, float* v) {
            float x = *v * 100.0f;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(label, &x, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) *v = x / 100.0f, changed = true;
        };
        // (turning an effect's settings turns it on)
        bool before = changed;
        auto shadow = [&](ShadowEffect& s, bool inner) {
            changed |= ColorEdit("Color", s.Color);
            changed |= ModeCombo("Blend Mode", s.Mode);
            percent("Opacity", &s.Opacity);
            slider("Angle", &s.Angle, -180.0f, 180.0f, "%.0f deg");
            slider("Distance", &s.Distance, 0.0f, 100.0f, "%.0f px");
            percent(inner ? "Choke" : "Spread", &s.Spread);
            slider("Size", &s.Size, 0.0f, 100.0f, "%.0f px");
        };
        auto glow = [&](GlowEffect& g, bool inner) {
            changed |= ColorEdit("Color", g.Color);
            changed |= ModeCombo("Blend Mode", g.Mode);
            percent("Opacity", &g.Opacity);
            percent(inner ? "Choke" : "Spread", &g.Spread);
            slider("Size", &g.Size, 0.0f, 100.0f, "%.0f px");
        };
        switch (section)
        {
        case 0: shadow(e.DropShadow, false); break;
        case 1: shadow(e.InnerShadow, true); break;
        case 2: glow(e.OuterGlow, false); break;
        case 3: glow(e.InnerGlow, true); break;
        case 4:
        {
            changed |= ColorEdit("Color", e.Stroke.Color);
            slider("Size", &e.Stroke.Size, 1.0f, 100.0f, "%.0f px");
            static const char* positions[] = { "Outside", "Inside", "Center" };
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::Combo("Position", &e.Stroke.Position, positions, 3);
            percent("Opacity", &e.Stroke.Opacity);
            break;
        }
        case 5:
            changed |= ColorEdit("Color", e.ColorOverlay.Color);
            changed |= ModeCombo("Blend Mode", e.ColorOverlay.Mode);
            percent("Opacity", &e.ColorOverlay.Opacity);
            break;
        case 6:
        {
            GradientEffect& g = e.GradientOverlay;
            changed |= ColorEdit("From", g.From);
            ImGui::SameLine();
            changed |= ColorEdit("To", g.To);
            if (ImGui::Button("From the Colors")) g.From = glm::vec3(m_Foreground), g.To = glm::vec3(m_Background), changed = true;
            changed |= ModeCombo("Blend Mode", g.Mode);
            percent("Opacity", &g.Opacity);
            slider("Angle", &g.Angle, -180.0f, 180.0f, "%.0f deg");
            changed |= ImGui::Checkbox("Reverse", &g.Reverse);
            break;
        }
        default: break;
        }
        if (changed && !before && !*on[section]) *on[section] = true;
        ImGui::EndChild();
        return changed;
    }
}
