#include "Dimension/UI/UI.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include <algorithm>

namespace Dimension::UI
{
    namespace
    {
        void LayoutRecursive(Scene& scene, Entity e, glm::vec2 parentMin, glm::vec2 parentMax, float scale, std::vector<Entity>& order)
        {
            auto& reg = scene.GetRegistry();
            if (!reg.Get<TransformComponent>(e).ActiveInHierarchy) return;
            glm::vec2 childMin = parentMin, childMax = parentMax;
            if (auto* rect = reg.TryGet<UIRectComponent>(e))
            {
                glm::vec2 parentSize = parentMax - parentMin;
                glm::vec2 anchorMin = parentMin + rect->AnchorMin * parentSize;
                glm::vec2 anchorMax = parentMin + rect->AnchorMax * parentSize;
                glm::vec2 size = (anchorMax - anchorMin) + rect->Size * scale;
                glm::vec2 pivotPos = glm::mix(anchorMin, anchorMax, rect->Pivot) + rect->Position * scale;
                rect->ComputedMin = pivotPos - rect->Pivot * size;
                rect->ComputedMax = rect->ComputedMin + size;
                rect->ComputedScale = scale;
                childMin = rect->ComputedMin;
                childMax = rect->ComputedMax;
                order.push_back(e);
            }
            for (Entity c : scene.GetChildren(e)) LayoutRecursive(scene, c, childMin, childMax, scale, order);
        }

        bool Contains(const UIRectComponent& r, const glm::vec2& p)
        {
            return p.x >= r.ComputedMin.x && p.y >= r.ComputedMin.y && p.x <= r.ComputedMax.x && p.y <= r.ComputedMax.y;
        }
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
            LayoutRecursive(scene, e, glm::vec2(0.0f), screen, scale, order);
        }
        return order;
    }

    Entity Update(Scene& scene, const PointerState& pointer, const glm::vec2& screen)
    {
        auto& reg = scene.GetRegistry();
        auto order = ComputeLayout(scene, screen);

        // Topmost element under the pointer (last drawn wins); images and buttons block, text does not.
        Entity hovered = Entity::Null(), hoveredButton = Entity::Null();
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            bool blocks = reg.Has<UIImageComponent>(*it) || reg.Has<UIButtonComponent>(*it);
            if (!blocks || !Contains(reg.Get<UIRectComponent>(*it), pointer.Position)) continue;
            hovered = *it;
            if (auto* b = reg.TryGet<UIButtonComponent>(*it); b && b->Interactable) hoveredButton = *it;
            break;
        }

        std::vector<Entity> clicked;
        for (auto [e, button] : reg.ViewOf<UIButtonComponent>())
        {
            button.Hovered = e == hoveredButton;
            if (pointer.Pressed && button.Hovered) button.Pressed = true;
            if (pointer.Released)
            {
                if (button.Pressed && button.Hovered) clicked.push_back(e);
                button.Pressed = false;
            }
        }
        for (Entity e : clicked) scene.DispatchUIClick(e); // after iteration: scripts may change the scene
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
            if (auto* image = reg.TryGet<UIImageComponent>(e))
            {
                glm::vec4 color = image->Color;
                if (auto* b = reg.TryGet<UIButtonComponent>(e))
                {
                    glm::vec4 tint = !b->Interactable ? glm::vec4(0.6f, 0.6f, 0.6f, 0.8f)
                                   : b->Pressed       ? b->PressedColor
                                   : b->Hovered       ? b->HoverColor
                                                      : b->NormalColor;
                    color *= tint;
                }
                auto tex = AssetManager::GetTexture(image->Texture);
                out.push_back({ rect.ComputedMin, rect.ComputedMax, { 0, 0 }, { 1, 1 }, color, tex ? tex->GetBindlessIndex() : white });
            }
            if (auto* text = reg.TryGet<UITextComponent>(e); text && !text->Text.empty())
            {
                auto font = AssetManager::GetFont(text->Font);
                if (!font) continue;
                glm::vec2 size = rect.ComputedMax - rect.ComputedMin;
                float pixelSize = text->FontSize * rect.ComputedScale;
                glm::vec2 block;
                auto quads = font->Layout(text->Text, pixelSize, text->Wrap ? size.x : 0.0f, text->Align, size.x, block);
                float y = text->VerticalAlign == TextVAlign::Top      ? 0.0f
                        : text->VerticalAlign == TextVAlign::Middle   ? (size.y - block.y) * 0.5f
                                                                      : size.y - block.y;
                glm::vec2 origin = rect.ComputedMin + glm::vec2(0.0f, y);
                uint32_t atlas = font->GetAtlas()->GetBindlessIndex();
                for (auto& q : quads) out.push_back({ origin + q.Min, origin + q.Max, q.UVMin, q.UVMax, text->Color, atlas });
            }
        }
    }
}
