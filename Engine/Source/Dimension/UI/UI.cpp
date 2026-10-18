#include "Dimension/UI/UI.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace Dimension::UI
{
    namespace
    {
        struct LayoutContext
        {
            Scene& TheScene;
            float Scale;
            std::vector<Entity>& Order;
        };

        void LayoutNode(LayoutContext& ctx, Entity e, glm::vec2 parentMin, glm::vec2 parentMax, glm::vec2 clipMin, glm::vec2 clipMax,
                        const glm::vec2* forcedMin, const glm::vec2* forcedMax);

        bool HasRect(Registry& reg, Entity e)
        {
            return reg.Has<UIRectComponent>(e) && reg.Get<TransformComponent>(e).ActiveInHierarchy;
        }

        // Children of a layout group are stacked along the main axis using their Size.
        void LayoutGroupChildren(LayoutContext& ctx, Entity e, const UILayoutGroupComponent& group, glm::vec2 min, glm::vec2 max,
                                 glm::vec2 clipMin, glm::vec2 clipMax)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            float s = ctx.Scale;
            glm::vec2 innerMin = min + glm::vec2(group.Padding.x, group.Padding.y) * s;
            glm::vec2 innerMax = max - glm::vec2(group.Padding.z, group.Padding.w) * s;
            bool vertical = group.Direction == UILayoutDirection::Vertical;
            float cursor = vertical ? innerMin.y : innerMin.x;
            for (Entity c : ctx.TheScene.GetChildren(e))
            {
                if (!HasRect(reg, c))
                {
                    LayoutNode(ctx, c, min, max, clipMin, clipMax, nullptr, nullptr);
                    continue;
                }
                auto& r = reg.Get<UIRectComponent>(c);
                glm::vec2 cmin, cmax;
                if (vertical)
                {
                    float h = r.Size.y * s;
                    float w = group.StretchChildren ? innerMax.x - innerMin.x : r.Size.x * s;
                    cmin = { innerMin.x, cursor };
                    cmax = { innerMin.x + w, cursor + h };
                    cursor += h + group.Spacing * s;
                }
                else
                {
                    float w = r.Size.x * s;
                    float h = group.StretchChildren ? innerMax.y - innerMin.y : r.Size.y * s;
                    cmin = { cursor, innerMin.y };
                    cmax = { cursor + w, innerMin.y + h };
                    cursor += w + group.Spacing * s;
                }
                LayoutNode(ctx, c, min, max, clipMin, clipMax, &cmin, &cmax);
            }
        }

        // Main-axis extent a layout group needs for its children (screen pixels).
        float LayoutGroupExtent(LayoutContext& ctx, Entity e, const UILayoutGroupComponent& group)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            bool vertical = group.Direction == UILayoutDirection::Vertical;
            float total = vertical ? group.Padding.y + group.Padding.w : group.Padding.x + group.Padding.z;
            int n = 0;
            for (Entity c : ctx.TheScene.GetChildren(e))
                if (HasRect(reg, c))
                {
                    auto& r = reg.Get<UIRectComponent>(c);
                    total += vertical ? r.Size.y : r.Size.x;
                    ++n;
                }
            if (n > 1) total += group.Spacing * (n - 1);
            return total * ctx.Scale;
        }

        void LayoutNode(LayoutContext& ctx, Entity e, glm::vec2 parentMin, glm::vec2 parentMax, glm::vec2 clipMin, glm::vec2 clipMax,
                        const glm::vec2* forcedMin, const glm::vec2* forcedMax)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            if (!reg.Get<TransformComponent>(e).ActiveInHierarchy) return;
            auto* rect = reg.TryGet<UIRectComponent>(e);
            if (!rect)
            {
                for (Entity c : ctx.TheScene.GetChildren(e)) LayoutNode(ctx, c, parentMin, parentMax, clipMin, clipMax, nullptr, nullptr);
                return;
            }

            float s = ctx.Scale;
            if (forcedMin)
            {
                rect->ComputedMin = *forcedMin;
                rect->ComputedMax = *forcedMax;
            }
            else
            {
                glm::vec2 parentSize = parentMax - parentMin;
                glm::vec2 anchorMin = parentMin + rect->AnchorMin * parentSize;
                glm::vec2 anchorMax = parentMin + rect->AnchorMax * parentSize;
                glm::vec2 size = (anchorMax - anchorMin) + rect->Size * s;
                glm::vec2 pivotPos = glm::mix(anchorMin, anchorMax, rect->Pivot) + rect->Position * s;
                rect->ComputedMin = pivotPos - rect->Pivot * size;
                rect->ComputedMax = rect->ComputedMin + size;
            }
            rect->ComputedScale = s;
            rect->ClipMin = clipMin;
            rect->ClipMax = clipMax;
            ctx.Order.push_back(e);

            auto* group = reg.TryGet<UILayoutGroupComponent>(e);
            if (auto* view = reg.TryGet<UIScrollViewComponent>(e))
            {
                // The view clips; its first UI child is the scrolled content. A vertical content layout group grows to fit its children.
                glm::vec2 viewMin = rect->ComputedMin, viewMax = rect->ComputedMax;
                glm::vec2 innerClipMin = glm::max(clipMin, viewMin), innerClipMax = glm::min(clipMax, viewMax);
                bool first = true;
                for (Entity c : ctx.TheScene.GetChildren(e))
                {
                    if (!first || !HasRect(reg, c))
                    {
                        LayoutNode(ctx, c, viewMin, viewMax, innerClipMin, innerClipMax, nullptr, nullptr);
                        continue;
                    }
                    first = false;
                    auto& cr = reg.Get<UIRectComponent>(c);
                    glm::vec2 parentSize = viewMax - viewMin;
                    glm::vec2 anchorMin = viewMin + cr.AnchorMin * parentSize, anchorMax = viewMin + cr.AnchorMax * parentSize;
                    glm::vec2 size = (anchorMax - anchorMin) + cr.Size * s;
                    glm::vec2 cmin = glm::mix(anchorMin, anchorMax, cr.Pivot) + cr.Position * s - cr.Pivot * size;
                    // Content grows downward from its top edge.
                    if (auto* cg = reg.TryGet<UILayoutGroupComponent>(c); cg && cg->Direction == UILayoutDirection::Vertical)
                        size.y = std::max(size.y, LayoutGroupExtent(ctx, c, *cg));
                    float maxScroll = std::max(0.0f, (cmin.y + size.y - viewMax.y) / s);
                    view->ScrollPosition = std::clamp(view->ScrollPosition, 0.0f, maxScroll);
                    cmin.y -= view->ScrollPosition * s;
                    glm::vec2 cmax = cmin + size;
                    LayoutNode(ctx, c, viewMin, viewMax, innerClipMin, innerClipMax, &cmin, &cmax);
                }
            }
            else if (group)
                LayoutGroupChildren(ctx, e, *group, rect->ComputedMin, rect->ComputedMax, clipMin, clipMax);
            else
                for (Entity c : ctx.TheScene.GetChildren(e))
                    LayoutNode(ctx, c, rect->ComputedMin, rect->ComputedMax, clipMin, clipMax, nullptr, nullptr);
        }

        bool Contains(const UIRectComponent& r, const glm::vec2& p)
        {
            glm::vec2 mn = glm::max(r.ComputedMin, r.ClipMin), mx = glm::min(r.ComputedMax, r.ClipMax);
            return p.x >= mn.x && p.y >= mn.y && p.x <= mx.x && p.y <= mx.y;
        }

        bool BlocksPointer(Registry& reg, Entity e)
        {
            return reg.Has<UIImageComponent>(e) || reg.Has<UIButtonComponent>(e) || reg.Has<UISliderComponent>(e) ||
                   reg.Has<UIToggleComponent>(e) || reg.Has<UIInputFieldComponent>(e);
        }

        void PopCodepoint(std::string& s)
        {
            if (s.empty()) return;
            size_t i = s.size() - 1;
            while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) --i;
            s.erase(i);
        }

        // Appends a clipped quad (UVs are cut proportionally).
        void Emit(std::vector<DrawCommand>& out, glm::vec2 mn, glm::vec2 mx, glm::vec2 uvMin, glm::vec2 uvMax, const glm::vec4& color,
                  uint32_t tex, const UIRectComponent& clip)
        {
            glm::vec2 cmn = glm::max(mn, clip.ClipMin), cmx = glm::min(mx, clip.ClipMax);
            if (cmx.x <= cmn.x || cmx.y <= cmn.y) return;
            glm::vec2 size = mx - mn;
            if (size.x <= 0.0f || size.y <= 0.0f) return;
            glm::vec2 t0 = (cmn - mn) / size, t1 = (cmx - mn) / size;
            out.push_back({ cmn, cmx, glm::mix(uvMin, uvMax, t0), glm::mix(uvMin, uvMax, t1), color, tex });
        }

        void EmitSliced(std::vector<DrawCommand>& out, const UIRectComponent& rect, const UIImageComponent& image, const glm::vec4& color,
                        const Ref<Texture2D>& tex, uint32_t texIndex)
        {
            glm::vec2 texSize((float)tex->GetWidth(), (float)tex->GetHeight());
            glm::vec2 mn = rect.ComputedMin, mx = rect.ComputedMax, size = mx - mn;
            float k = image.BorderScale * rect.ComputedScale;
            glm::vec4 b = image.Border * k; // screen-space borders (l, t, r, b)
            // Shrink borders that do not fit.
            float fx = (b.x + b.z) > size.x && (b.x + b.z) > 0 ? size.x / (b.x + b.z) : 1.0f;
            float fy = (b.y + b.w) > size.y && (b.y + b.w) > 0 ? size.y / (b.y + b.w) : 1.0f;
            b.x *= fx; b.z *= fx; b.y *= fy; b.w *= fy;
            float xs[4] = { mn.x, mn.x + b.x, mx.x - b.z, mx.x };
            float ys[4] = { mn.y, mn.y + b.y, mx.y - b.w, mx.y };
            glm::vec4 ub = image.Border / glm::vec4(texSize, texSize);
            float us[4] = { 0.0f, ub.x, 1.0f - ub.z, 1.0f };
            float vs[4] = { 0.0f, ub.y, 1.0f - ub.w, 1.0f };
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x)
                    Emit(out, { xs[x], ys[y] }, { xs[x + 1], ys[y + 1] }, { us[x], vs[y] }, { us[x + 1], vs[y + 1] }, color, texIndex, rect);
        }

        // Lays out a single line of text vertically centred in boxHeight; returns its width.
        float EmitText(std::vector<DrawCommand>& out, const Font& font, const std::string& text, float pixelSize, glm::vec2 origin,
                       float boxHeight, const glm::vec4& color, const UIRectComponent& clip)
        {
            glm::vec2 block;
            auto quads = font.Layout(text, pixelSize, 0.0f, TextAlign::Left, 0.0f, block);
            glm::vec2 o = origin + glm::vec2(0.0f, (boxHeight - block.y) * 0.5f);
            uint32_t atlas = font.GetAtlas()->GetBindlessIndex();
            for (auto& q : quads) Emit(out, o + q.Min, o + q.Max, q.UVMin, q.UVMax, color, atlas, clip);
            return block.x;
        }

        float TextWidth(const Font& font, const std::string& text, float pixelSize)
        {
            float w = 0.0f;
            for (unsigned char c : text)
                if (c >= Font::kFirstChar && (c & 0xC0) != 0x80) w += font.GetGlyph(c).Advance;
            return w * pixelSize / Font::kBakeSize;
        }

        constexpr glm::vec4 kDisabledTint{ 0.6f, 0.6f, 0.6f, 0.8f };
        constexpr glm::vec4 kPressedTint{ 0.85f, 0.85f, 0.85f, 1.0f };
    }

    size_t CodepointCount(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    std::vector<Entity> ComputeLayout(Scene& scene, const glm::vec2& screen)
    {
        auto& reg = scene.GetRegistry();
        std::vector<std::pair<int32_t, Entity>> canvases;
        for (auto [e, canvas, t] : reg.ViewOf<CanvasComponent, TransformComponent>())
            if (t.ActiveInHierarchy) canvases.emplace_back(canvas.SortingOrder, e);
        std::stable_sort(canvases.begin(), canvases.end(), [](auto& a, auto& b) { return a.first < b.first; });

        std::vector<Entity> order;
        for (auto& [sortOrder, e] : canvases)
        {
            auto& canvas = reg.Get<CanvasComponent>(e);
            float scale = 1.0f;
            if (canvas.ScaleMode == CanvasScaleMode::ScaleWithScreenSize && canvas.ReferenceResolution.x > 0 && canvas.ReferenceResolution.y > 0)
                scale = std::min(screen.x / canvas.ReferenceResolution.x, screen.y / canvas.ReferenceResolution.y);
            LayoutContext ctx{ scene, scale, order };
            LayoutNode(ctx, e, glm::vec2(0.0f), screen, glm::vec2(0.0f), screen, nullptr, nullptr);
        }
        return order;
    }

    Entity Update(Scene& scene, const PointerState& pointer, const glm::vec2& screen, float dt)
    {
        auto& reg = scene.GetRegistry();
        auto order = ComputeLayout(scene, screen);

        // Topmost element under the pointer (last drawn wins); widgets and images block, text does not.
        Entity hovered = Entity::Null();
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if (BlocksPointer(reg, *it) && Contains(reg.Get<UIRectComponent>(*it), pointer.Position))
            {
                hovered = *it;
                break;
            }

        // The wheel scrolls the innermost scroll view under the pointer (clamped by the next layout).
        if (pointer.Scroll != 0.0f)
            for (auto it = order.rbegin(); it != order.rend(); ++it)
                if (auto* view = reg.TryGet<UIScrollViewComponent>(*it); view && Contains(reg.Get<UIRectComponent>(*it), pointer.Position))
                {
                    view->ScrollPosition = std::max(0.0f, view->ScrollPosition - pointer.Scroll * view->ScrollSpeed);
                    break;
                }

        // Events are collected and dispatched after iteration: scripts may change the scene.
        std::vector<Entity> clicked;
        std::vector<std::pair<Entity, float>> values;
        std::vector<std::tuple<Entity, std::string, bool>> texts;

        for (auto [e, button] : reg.ViewOf<UIButtonComponent>())
        {
            button.Hovered = e == hovered && button.Interactable;
            if (pointer.Pressed && button.Hovered) button.Pressed = true;
            if (pointer.Released)
            {
                if (button.Pressed && button.Hovered) clicked.push_back(e);
                button.Pressed = false;
            }
        }

        for (auto [e, slider] : reg.ViewOf<UISliderComponent>())
        {
            if (pointer.Pressed && e == hovered && slider.Interactable) slider.Dragging = true;
            if (!pointer.Down) slider.Dragging = false;
            if (!slider.Dragging) continue;
            auto& r = reg.Get<UIRectComponent>(e);
            float radius = (r.ComputedMax.y - r.ComputedMin.y) * 0.5f;
            float width = std::max(1.0f, (r.ComputedMax.x - r.ComputedMin.x) - 2.0f * radius);
            float t = std::clamp((pointer.Position.x - r.ComputedMin.x - radius) / width, 0.0f, 1.0f);
            float v = slider.MinValue + t * (slider.MaxValue - slider.MinValue);
            if (slider.WholeNumbers) v = std::round(v);
            if (v != slider.Value)
            {
                slider.Value = v;
                values.emplace_back(e, v);
            }
        }

        for (auto [e, toggle] : reg.ViewOf<UIToggleComponent>())
        {
            bool over = e == hovered && toggle.Interactable;
            if (pointer.Pressed && over) toggle.Pressed = true;
            if (pointer.Released)
            {
                if (toggle.Pressed && over)
                {
                    toggle.IsOn = !toggle.IsOn;
                    values.emplace_back(e, toggle.IsOn ? 1.0f : 0.0f);
                }
                toggle.Pressed = false;
            }
        }

        for (auto [e, field] : reg.ViewOf<UIInputFieldComponent>())
        {
            if (pointer.Pressed)
            {
                bool focus = e == hovered && field.Interactable;
                if (focus && !field.Focused) field.CaretTime = 0.0f;
                field.Focused = focus;
            }
            if (!field.Focused) continue;
            field.CaretTime += dt;
            std::string before = field.Text;
            for (size_t i = 0; i < pointer.Text.size();)
            {
                unsigned char c = (unsigned char)pointer.Text[i];
                size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
                bool printable = c >= 32 && c != 127;
                if (printable && (field.CharacterLimit <= 0 || CodepointCount(field.Text) < (size_t)field.CharacterLimit))
                    field.Text.append(pointer.Text, i, len);
                i += len;
            }
            if (pointer.Backspace) PopCodepoint(field.Text);
            if (field.Text != before)
            {
                field.CaretTime = 0.0f;
                texts.emplace_back(e, field.Text, false);
            }
            if (pointer.Enter)
            {
                field.Focused = false;
                texts.emplace_back(e, field.Text, true);
            }
        }

        for (Entity e : clicked) scene.DispatchUIClick(e);
        for (auto& [e, v] : values) scene.DispatchUIValueChanged(e, v);
        for (auto& [e, text, submit] : texts) scene.DispatchUIText(e, text, submit);
        return hovered;
    }

    void BuildDrawList(Scene& scene, const glm::vec2& screen, std::vector<DrawCommand>& out)
    {
        auto& reg = scene.GetRegistry();
        auto order = ComputeLayout(scene, screen);
        uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();

        for (Entity e : order)
        {
            auto& rect = reg.Get<UIRectComponent>(e);
            glm::vec2 mn = rect.ComputedMin, mx = rect.ComputedMax, size = mx - mn;
            float s = rect.ComputedScale;

            if (auto* image = reg.TryGet<UIImageComponent>(e))
            {
                glm::vec4 color = image->Color;
                if (auto* b = reg.TryGet<UIButtonComponent>(e))
                {
                    glm::vec4 tint = !b->Interactable ? kDisabledTint
                                   : b->Pressed       ? b->PressedColor
                                   : b->Hovered       ? b->HoverColor
                                                      : b->NormalColor;
                    color *= tint;
                }
                auto tex = AssetManager::GetTexture(image->Texture);
                uint32_t index = tex ? tex->GetBindlessIndex() : white;
                if (image->Type == UIImageType::Sliced && tex)
                    EmitSliced(out, rect, *image, color, tex, index);
                else
                    Emit(out, mn, mx, { 0, 0 }, { 1, 1 }, color, index, rect);
            }

            if (auto* slider = reg.TryGet<UISliderComponent>(e))
            {
                float radius = size.y * 0.5f;
                float range = slider->MaxValue - slider->MinValue;
                float t = range != 0.0f ? std::clamp((slider->Value - slider->MinValue) / range, 0.0f, 1.0f) : 0.0f;
                float trackH = std::max(2.0f, size.y * 0.3f);
                float cy = mn.y + radius;
                float x0 = mn.x + radius, x1 = mx.x - radius;
                float hx = x0 + (x1 - x0) * t;
                glm::vec4 dim = slider->Interactable ? glm::vec4(1.0f) : kDisabledTint;
                Emit(out, { x0, cy - trackH * 0.5f }, { x1, cy + trackH * 0.5f }, { 0, 0 }, { 1, 1 }, slider->BackgroundColor * dim, white, rect);
                Emit(out, { x0, cy - trackH * 0.5f }, { hx, cy + trackH * 0.5f }, { 0, 0 }, { 1, 1 }, slider->FillColor * dim, white, rect);
                uint32_t circle = AssetManager::GetCircleTexture()->GetBindlessIndex();
                glm::vec4 handle = slider->HandleColor * dim;
                if (slider->Dragging) handle *= kPressedTint;
                Emit(out, { hx - radius, mn.y }, { hx + radius, mx.y }, { 0, 0 }, { 1, 1 }, handle, circle, rect);
            }

            if (auto* toggle = reg.TryGet<UIToggleComponent>(e))
            {
                float box = size.y;
                glm::vec4 dim = toggle->Interactable ? glm::vec4(1.0f) : kDisabledTint;
                glm::vec4 boxColor = toggle->BoxColor * dim;
                if (toggle->Pressed) boxColor *= kPressedTint;
                Emit(out, mn, mn + glm::vec2(box), { 0, 0 }, { 1, 1 }, boxColor, white, rect);
                if (toggle->IsOn)
                {
                    float inset = box * 0.22f;
                    Emit(out, mn + glm::vec2(inset), mn + glm::vec2(box - inset), { 0, 0 }, { 1, 1 }, toggle->CheckColor * dim, white, rect);
                }
                if (!toggle->Label.empty())
                    if (auto font = AssetManager::GetDefaultFont())
                        EmitText(out, *font, toggle->Label, toggle->FontSize * s, { mn.x + box + 10.0f * s, mn.y }, size.y,
                                 toggle->LabelColor * dim, rect);
            }

            if (auto* field = reg.TryGet<UIInputFieldComponent>(e))
            {
                if (!reg.Has<UIImageComponent>(e))
                    Emit(out, mn, mx, { 0, 0 }, { 1, 1 }, field->BackgroundColor, white, rect);
                if (auto font = AssetManager::GetDefaultFont())
                {
                    float pad = 10.0f * s, px = field->FontSize * s;
                    UIRectComponent inner = rect; // text is clipped to the padded field
                    inner.ClipMin = glm::max(rect.ClipMin, mn + glm::vec2(pad * 0.5f, 0.0f));
                    inner.ClipMax = glm::min(rect.ClipMax, mx - glm::vec2(pad * 0.5f, 0.0f));
                    float avail = size.x - 2.0f * pad;
                    bool empty = field->Text.empty();
                    float width = empty ? 0.0f : TextWidth(*font, field->Text, px);
                    float shift = std::max(0.0f, width - avail); // keep the caret end visible
                    glm::vec2 origin{ mn.x + pad - shift, mn.y };
                    if (empty && !field->Placeholder.empty() && !field->Focused)
                        EmitText(out, *font, field->Placeholder, px, origin, size.y, field->PlaceholderColor, inner);
                    else if (!empty)
                        EmitText(out, *font, field->Text, px, origin, size.y, field->TextColor, inner);
                    if (field->Focused && std::fmod(field->CaretTime, 1.0f) < 0.55f)
                    {
                        float cx = origin.x + width + 1.0f * s;
                        float h = px * 1.1f;
                        float cy = mn.y + (size.y - h) * 0.5f;
                        Emit(out, { cx, cy }, { cx + std::max(1.0f, 2.0f * s), cy + h }, { 0, 0 }, { 1, 1 }, field->TextColor, white, inner);
                    }
                }
            }

            if (auto* text = reg.TryGet<UITextComponent>(e); text && !text->Text.empty())
            {
                auto font = AssetManager::GetFont(text->Font);
                if (!font) continue;
                float pixelSize = text->FontSize * s;
                glm::vec2 block;
                auto quads = font->Layout(text->Text, pixelSize, text->Wrap ? size.x : 0.0f, text->Align, size.x, block);
                float y = text->VerticalAlign == TextVAlign::Top      ? 0.0f
                        : text->VerticalAlign == TextVAlign::Middle   ? (size.y - block.y) * 0.5f
                                                                      : size.y - block.y;
                glm::vec2 origin = mn + glm::vec2(0.0f, y);
                uint32_t atlas = font->GetAtlas()->GetBindlessIndex();
                for (auto& q : quads) Emit(out, origin + q.Min, origin + q.Max, q.UVMin, q.UVMax, text->Color, atlas, rect);
            }
        }
    }
}
