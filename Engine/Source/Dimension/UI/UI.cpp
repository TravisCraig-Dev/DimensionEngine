#include "Dimension/UI/UI.h"
#include "Dimension/Scene/CameraProjection.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Project/Localization.h"
#include "Dimension/Renderer/Font.h"
#include "Dimension/Renderer/Texture.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include <format>
#include <unordered_set>
#include <algorithm>
#include <cctype>
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

        // Columns of a grid layout whose inner width is `innerWidth` canvas units.
        int GridColumns(const UILayoutGroupComponent& group, float innerWidth)
        {
            if (group.Columns > 0) return group.Columns;
            float step = std::max(group.CellSize.x + group.Spacing, 1.0f);
            return std::max(1, (int)std::floor((innerWidth + group.Spacing) / step + 1e-4f));
        }

        // A child a layout group places (not one with Ignore Layout).
        bool InLayout(Registry& reg, Entity c)
        {
            if (!HasRect(reg, c)) return false;
            auto* le = reg.TryGet<UILayoutElementComponent>(c);
            return !le || !le->IgnoreLayout;
        }

        // Content and layout-group sizes (defined below, after the text helpers).
        glm::vec2 ContentSize(LayoutContext& ctx, Entity e, float wrapWidth);
        glm::vec2 ElementSize(LayoutContext& ctx, Entity e, float crossWidth);
        void FlowLines(LayoutContext& ctx, Entity e, const UILayoutGroupComponent& group, glm::vec2 inner,
                       std::vector<glm::vec2>& outMin, std::vector<glm::vec2>& outSize);

        float FlexibleWeight(Registry& reg, Entity e, int axis)
        {
            auto* le = reg.TryGet<UILayoutElementComponent>(e);
            return le ? std::max(0.0f, le->FlexibleSize[axis]) : 0.0f;
        }

        // A row or column group's children in lines (canvas units, from the inner min; `inner`: the inner size): sizes
        // from ElementSize, a new line when Wrap is on and the next child does not fit, each line's leftover length
        // shared by flexible weight or else placed by Justify, children across their line by Align (or stretched).
        void FlowLines(LayoutContext& ctx, Entity e, const UILayoutGroupComponent& group, glm::vec2 inner,
                       std::vector<glm::vec2>& outMin, std::vector<glm::vec2>& outSize)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            int m = group.Direction == UILayoutDirection::Vertical ? 1 : 0, c = 1 - m;
            float crossWidth = m == 1 && group.StretchChildren && !group.Wrap ? inner.x : -1.0f;
            std::vector<Entity> kids;
            std::vector<glm::vec2> sizes;
            for (Entity k : ctx.TheScene.GetChildren(e))
                if (InLayout(reg, k))
                {
                    kids.push_back(k);
                    sizes.push_back(ElementSize(ctx, k, crossWidth));
                }
            outMin.assign(kids.size(), glm::vec2(0.0f));
            outSize = sizes;
            float crossCursor = 0.0f;
            for (size_t first = 0; first < kids.size();)
            {
                // The line: as many as fit (at least one).
                size_t last = first + 1;
                float used = sizes[first][m];
                if (group.Wrap)
                    while (last < kids.size() && used + group.Spacing + sizes[last][m] <= inner[m] + 1e-3f) used += group.Spacing + sizes[last++][m];
                else
                    for (; last < kids.size(); ++last) used += group.Spacing + sizes[last][m];
                size_t n = last - first;
                float weights = 0.0f, lineCross = 0.0f;
                for (size_t i = first; i < last; ++i)
                {
                    weights += FlexibleWeight(reg, kids[i], m);
                    lineCross = std::max(lineCross, sizes[i][c]);
                }
                if (!group.Wrap) lineCross = inner[c]; // one line: the whole group across
                float left = inner[m] - used;
                if (left > 0.0f && weights > 0.0f)
                {
                    for (size_t i = first; i < last; ++i) outSize[i][m] += left * FlexibleWeight(reg, kids[i], m) / weights;
                    left = 0.0f;
                }
                left = std::max(left, 0.0f);
                float cursor = group.Justify == UIJustify::Center ? left * 0.5f : group.Justify == UIJustify::End ? left : 0.0f;
                float gap = group.Spacing + (group.Justify == UIJustify::SpaceBetween && n > 1 ? left / (float)(n - 1) : 0.0f);
                for (size_t i = first; i < last; ++i)
                {
                    float cross = group.StretchChildren ? lineCross : outSize[i][c];
                    outSize[i][c] = cross;
                    float offset = group.Align == UIAlign::Center ? (lineCross - cross) * 0.5f : group.Align == UIAlign::End ? lineCross - cross : 0.0f;
                    outMin[i][m] = cursor;
                    outMin[i][c] = crossCursor + offset;
                    cursor += outSize[i][m] + gap;
                }
                crossCursor += lineCross + group.Spacing;
                first = last;
            }
        }

        // Children of a layout group are stacked along the main axis using their sizes (see ElementSize); flexible ones
        // share what is left of the group's length.
        void LayoutGroupChildren(LayoutContext& ctx, Entity e, const UILayoutGroupComponent& group, glm::vec2 min, glm::vec2 max,
                                 glm::vec2 clipMin, glm::vec2 clipMax)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            float s = ctx.Scale;
            glm::vec2 innerMin = min + glm::vec2(group.Padding.x, group.Padding.y) * s;
            glm::vec2 innerMax = max - glm::vec2(group.Padding.z, group.Padding.w) * s;
            bool grid = group.Direction == UILayoutDirection::Grid;
            int columns = grid ? GridColumns(group, (innerMax.x - innerMin.x) / s) : 0;
            int index = 0;
            // Rows and columns: lines of children (one unless wrapping), each child's rect in canvas units from the inner min.
            std::vector<glm::vec2> rectMin, rectSize;
            if (!grid) FlowLines(ctx, e, group, (innerMax - innerMin) / s, rectMin, rectSize);
            size_t slot = 0;
            for (Entity c : ctx.TheScene.GetChildren(e))
            {
                if (!InLayout(reg, c))
                {
                    LayoutNode(ctx, c, min, max, clipMin, clipMax, nullptr, nullptr);
                    continue;
                }
                glm::vec2 cmin, cmax;
                if (grid)
                {
                    int col = index % columns, row = index / columns;
                    ++index;
                    cmin = innerMin + glm::vec2(col * (group.CellSize.x + group.Spacing), row * (group.CellSize.y + group.Spacing)) * s;
                    cmax = cmin + group.CellSize * s;
                }
                else
                {
                    cmin = innerMin + rectMin[slot] * s;
                    cmax = cmin + rectSize[slot] * s;
                    ++slot;
                }
                LayoutNode(ctx, c, min, max, clipMin, clipMax, &cmin, &cmax);
            }
        }

        // Main-axis extent a layout group needs for its children (screen pixels): height for columns and grids
        // (`width` is the group's width in pixels, which sets a flexible grid's column count), width for rows.
        float LayoutGroupExtent(LayoutContext& ctx, Entity e, const UILayoutGroupComponent& group, float width)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            if (group.Direction == UILayoutDirection::Grid)
            {
                int n = 0;
                for (Entity c : ctx.TheScene.GetChildren(e)) n += InLayout(reg, c) ? 1 : 0;
                int columns = GridColumns(group, width / ctx.Scale - group.Padding.x - group.Padding.z);
                int rows = (n + columns - 1) / columns;
                float h = group.Padding.y + group.Padding.w + rows * group.CellSize.y + std::max(0, rows - 1) * group.Spacing;
                return h * ctx.Scale;
            }
            if (group.Wrap)
            {
                // Lines laid out across the width: their total height (rows) or width (columns: its height is the limit).
                std::vector<glm::vec2> mins, sizes;
                glm::vec2 inner(width / ctx.Scale - group.Padding.x - group.Padding.z, 1e9f);
                if (group.Direction == UILayoutDirection::Vertical) inner = { 1e9f, 1e9f };
                FlowLines(ctx, e, group, inner, mins, sizes);
                float extent = 0.0f;
                int axis = group.Direction == UILayoutDirection::Vertical ? 0 : 1;
                for (size_t i = 0; i < mins.size(); ++i) extent = std::max(extent, mins[i][axis] + sizes[i][axis]);
                return (extent + (axis ? group.Padding.y + group.Padding.w : group.Padding.x + group.Padding.z)) * ctx.Scale;
            }
            bool vertical = group.Direction == UILayoutDirection::Vertical;
            float total = vertical ? group.Padding.y + group.Padding.w : group.Padding.x + group.Padding.z;
            int n = 0;
            float crossWidth = vertical && group.StretchChildren ? width / ctx.Scale - group.Padding.x - group.Padding.z : -1.0f;
            for (Entity c : ctx.TheScene.GetChildren(e))
                if (InLayout(reg, c))
                {
                    glm::vec2 size = ElementSize(ctx, c, crossWidth);
                    total += vertical ? size.y : size.x;
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
                if (auto* fit = reg.TryGet<UIContentSizeFitterComponent>(e))
                {
                    // Sized to its content (about its pivot).
                    bool fitX = fit->Horizontal == UIFitMode::PreferredSize, fitY = fit->Vertical == UIFitMode::PreferredSize;
                    if (fitX || fitY)
                    {
                        glm::vec2 content = ContentSize(ctx, e, fitX ? -1.0f : size.x / s);
                        if (fitX) size.x = content.x * s;
                        if (fitY) size.y = content.y * s;
                    }
                }
                glm::vec2 pivotPos = glm::mix(anchorMin, anchorMax, rect->Pivot) + rect->Position * s;
                rect->ComputedMin = pivotPos - rect->Pivot * size;
                rect->ComputedMax = rect->ComputedMin + size;
            }
            rect->ComputedScale = s;
            rect->ClipMin = clipMin;
            rect->ClipMax = clipMax;
            ctx.Order.push_back(e);
            if (reg.Has<UIMaskComponent>(e))
            {
                // A mask clips what is under it to its rect (its shape and soft edges are applied when drawn).
                clipMin = glm::max(clipMin, rect->ComputedMin);
                clipMax = glm::min(clipMax, rect->ComputedMax);
            }

            auto* group = reg.TryGet<UILayoutGroupComponent>(e);
            if (auto* view = reg.TryGet<UIScrollViewComponent>(e))
            {
                // The view clips; its first UI child is the scrolled content. A vertical content layout group grows to fit its children.
                glm::vec2 viewMin = rect->ComputedMin, viewMax = rect->ComputedMax;
                glm::vec2 innerClipMin = glm::max(clipMin, viewMin), innerClipMax = glm::min(clipMax, viewMax);
                bool first = true;
                view->MaxScroll = view->MaxScrollX = 0.0f;
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
                    // Content grows downward from its top edge (columns, grids) or rightward from its left edge (rows).
                    if (auto* cg = reg.TryGet<UILayoutGroupComponent>(c))
                    {
                        if (cg->Direction == UILayoutDirection::Horizontal && !cg->Wrap) size.x = std::max(size.x, LayoutGroupExtent(ctx, c, *cg, size.x));
                        else size.y = std::max(size.y, LayoutGroupExtent(ctx, c, *cg, size.x));
                    }
                    float maxScroll = view->Vertical ? std::max(0.0f, (cmin.y + size.y - viewMax.y) / s) : 0.0f;
                    float maxScrollX = view->Horizontal ? std::max(0.0f, (cmin.x + size.x - viewMax.x) / s) : 0.0f;
                    view->MaxScroll = maxScroll;
                    view->MaxScrollX = maxScrollX;
                    view->ScrollPosition = std::clamp(view->ScrollPosition, 0.0f, maxScroll);
                    view->ScrollPositionX = std::clamp(view->ScrollPositionX, 0.0f, maxScrollX);
                    cmin.y -= view->ScrollPosition * s;
                    cmin.x -= view->ScrollPositionX * s;
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
                   reg.Has<UIToggleComponent>(e) || reg.Has<UIInputFieldComponent>(e) || reg.Has<UIDropdownComponent>(e);
        }

        bool InBox(const glm::vec2& p, const glm::vec2& mn, const glm::vec2& mx)
        {
            return p.x >= mn.x && p.y >= mn.y && p.x <= mx.x && p.y <= mx.y;
        }

        // Scrollbar along a scroll view's right edge (vertical) or bottom edge (horizontal), in screen pixels; false
        // while the content fits on that axis. When both show, each stops short of the corner they share.
        struct ScrollbarGeometry
        {
            glm::vec2 TrackMin, TrackMax, HandleMin, HandleMax;
        };

        bool GetScrollbar(const UIRectComponent& r, const UIScrollViewComponent& view, ScrollbarGeometry& g, bool horizontal = false)
        {
            float maxScroll = horizontal ? view.MaxScrollX : view.MaxScroll;
            if (!view.Scrollbar || maxScroll <= 0.0f) return false;
            float s = r.ComputedScale, inset = 4.0f * s, w = std::max(2.0f, view.ScrollbarWidth * s);
            int axis = horizontal ? 0 : 1;
            bool other = horizontal ? view.MaxScroll > 0.0f : view.MaxScrollX > 0.0f; // the other bar takes the corner
            float cornerGap = other ? w + inset : 0.0f;
            if (horizontal)
            {
                g.TrackMin = { r.ComputedMin.x + inset, r.ComputedMax.y - inset - w };
                g.TrackMax = { r.ComputedMax.x - inset - cornerGap, r.ComputedMax.y - inset };
            }
            else
            {
                g.TrackMin = { r.ComputedMax.x - inset - w, r.ComputedMin.y + inset };
                g.TrackMax = { r.ComputedMax.x - inset, r.ComputedMax.y - inset - cornerGap };
            }
            float track = g.TrackMax[axis] - g.TrackMin[axis];
            if (track <= 0.0f) return false;
            float viewLen = (r.ComputedMax[axis] - r.ComputedMin[axis]) / s; // canvas units, like MaxScroll
            float handle = std::clamp(track * viewLen / (viewLen + maxScroll), std::min(24.0f * s, track), track);
            float t = std::clamp((horizontal ? view.ScrollPositionX : view.ScrollPosition) / maxScroll, 0.0f, 1.0f);
            g.HandleMin = g.TrackMin;
            g.HandleMax = g.TrackMax;
            g.HandleMin[axis] = g.TrackMin[axis] + (track - handle) * t;
            g.HandleMax[axis] = g.HandleMin[axis] + handle;
            return true;
        }

        // The open list of a dropdown: below the box, or above it when it does not fit on screen.
        struct DropdownList
        {
            glm::vec2 Min, Max;
            float ItemHeight;
            int Visible;
        };

        DropdownList GetDropdownList(const UIRectComponent& r, const UIDropdownComponent& d, const glm::vec2& screen)
        {
            DropdownList l;
            l.ItemHeight = std::max(1.0f, r.ComputedMax.y - r.ComputedMin.y);
            int n = (int)d.Options.size();
            l.Visible = std::min(n, std::max(1, d.MaxVisibleItems));
            float h = l.Visible * l.ItemHeight;
            bool below = r.ComputedMax.y + h <= screen.y || r.ComputedMin.y - h < 0.0f;
            l.Min = { r.ComputedMin.x, below ? r.ComputedMax.y : r.ComputedMin.y - h };
            l.Max = { r.ComputedMax.x, l.Min.y + h };
            return l;
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
                  uint32_t tex, const UIRectComponent& clip, float slant = 0.0f)
        {
            glm::vec2 cmn = glm::max(mn, clip.ClipMin), cmx = glm::min(mx, clip.ClipMax);
            if (cmx.x <= cmn.x || cmx.y <= cmn.y) return;
            glm::vec2 size = mx - mn;
            if (size.x <= 0.0f || size.y <= 0.0f) return;
            glm::vec2 t0 = (cmn - mn) / size, t1 = (cmx - mn) / size;
            DrawCommand cmd{ cmn, cmx, glm::mix(uvMin, uvMax, t0), glm::mix(uvMin, uvMax, t1), color, tex, slant };
            if (slant != 0.0f) // a bottom clipped off moves the new bottom edge along the slant
            {
                float shift = slant * (mx.y - cmx.y);
                cmd.Min.x += shift;
                cmd.Max.x += shift;
            }
            out.push_back(cmd);
        }

        // Rich text quads: glyphs in the atlas, underlines and strikethroughs in white, tag colors replacing the
        // text's color (keeping its alpha).
        void EmitTextQuads(std::vector<DrawCommand>& out, const std::vector<TextQuad>& quads, glm::vec2 origin, const glm::vec4& color, uint32_t atlas,
                           uint32_t white, const UIRectComponent& clip)
        {
            for (auto& q : quads)
            {
                glm::vec4 c = q.HasColor ? glm::vec4(glm::vec3(q.Color), q.Color.a * color.a) : color;
                Emit(out, origin + q.Min, origin + q.Max, q.UVMin, q.UVMax, c, q.Solid ? white : atlas, clip, q.Skew);
            }
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
            uint32_t atlas = font.GetAtlasIndex();
            for (auto& q : quads) Emit(out, o + q.Min, o + q.Max, q.UVMin, q.UVMax, color, atlas, clip);
            return block.x;
        }

        size_t LineOf(const std::vector<size_t>& starts, size_t offset)
        {
            return (size_t)(std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin()) - 1;
        }

        float TextWidth(const Font& font, const std::string& text, float pixelSize)
        {
            float w = 0.0f;
            for (unsigned char c : text)
                if (c >= Font::kFirstChar && (c & 0xC0) != 0x80) w += font.GetGlyph(c).Advance;
            return w * pixelSize / Font::kBakeSize;
        }

        // Small solid triangle built from stacked rows (UI quads are axis-aligned).
        void EmitArrow(std::vector<DrawCommand>& out, glm::vec2 center, float width, bool up, const glm::vec4& color, uint32_t white,
                       const UIRectComponent& clip)
        {
            constexpr int kRows = 8;
            float height = width * 0.55f, rowH = height / kRows;
            for (int i = 0; i < kRows; ++i)
            {
                float f = (float)i / kRows; // 0 = wide end
                float w = width * (1.0f - f);
                float y = up ? center.y + height * 0.5f - (i + 1) * rowH : center.y - height * 0.5f + i * rowH;
                Emit(out, { center.x - w * 0.5f, y }, { center.x + w * 0.5f, y + rowH + 0.5f }, { 0, 0 }, { 1, 1 }, color, white, clip);
            }
        }

        // ---- Text editing helpers (byte offsets on UTF-8 boundaries) ----
        size_t NextCodepoint(const std::string& s, size_t i)
        {
            if (i >= s.size()) return s.size();
            ++i;
            while (i < s.size() && ((uint8_t)s[i] & 0xC0) == 0x80) ++i;
            return i;
        }

        // Visual lines of multi-line text, [Starts[i], Ends[i]) each. Lines break at '\n' and, with a wrap width
        // (pixels), before the word that would overflow; a word longer than the width breaks between characters.
        // A wrapped line keeps its trailing space, so then Ends[i] == Starts[i + 1].
        struct TextLines
        {
            std::vector<size_t> Starts, Ends;
            bool Wrapped(size_t line) const { return line + 1 < Starts.size() && Ends[line] == Starts[line + 1]; }
        };
        TextLines LayoutLines(const std::string& s, float wrapWidth, float px)
        {
            TextLines lines;
            for (size_t para = 0;;)
            {
                size_t nl = s.find('\n', para), paraEnd = nl == std::string::npos ? s.size() : nl, start = para;
                while (wrapWidth > 0.0f && start < paraEnd && MeasureText(s.substr(start, paraEnd - start), px) > wrapWidth)
                {
                    // Longest prefix that fits (spaces may hang past the edge), preferring to end after a space.
                    size_t fit = start, afterSpace = std::string::npos;
                    for (size_t i = start; i < paraEnd; i = NextCodepoint(s, i))
                    {
                        size_t next = NextCodepoint(s, i);
                        if (s[i] != ' ' && MeasureText(s.substr(start, next - start), px) > wrapWidth) break;
                        fit = next;
                        if (s[i] == ' ') afterSpace = next;
                    }
                    size_t brk = afterSpace != std::string::npos ? afterSpace : std::max(fit, NextCodepoint(s, start));
                    if (brk >= paraEnd) break;
                    lines.Starts.push_back(start);
                    lines.Ends.push_back(brk);
                    start = brk;
                }
                lines.Starts.push_back(start);
                lines.Ends.push_back(paraEnd);
                if (nl == std::string::npos) break;
                para = nl + 1;
            }
            return lines;
        }

        std::string StripRichText(const std::string& s)
        {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '<')
                    if (size_t close = s.find('>', i); close != std::string::npos)
                    {
                        if (s.compare(i, close - i + 1, "<br>") == 0) out += '\n';
                        i = close;
                        continue;
                    }
                out += s[i];
            }
            return out;
        }

        // A UI Text's size in canvas units: its widest line and its lines' height (wrapped at `wrapWidth` units when
        // it wraps and that is positive).
        glm::vec2 TextSize(const UITextComponent& text, float wrapWidth)
        {
            std::string s = text.LocalizedKey.empty() ? text.Text : Localization::Get(text.LocalizedKey);
            if (text.RichText) s = StripRichText(s);
            float px = text.FontSize;
            TextLines lines = LayoutLines(s, text.Wrap && wrapWidth > 0.0f ? wrapWidth : 0.0f, px);
            float width = 0.0f;
            for (size_t i = 0; i < lines.Starts.size(); ++i) width = std::max(width, MeasureText(s.substr(lines.Starts[i], lines.Ends[i] - lines.Starts[i]), px));
            float lineH = px * 1.2f;
            if (RenderDevice::Exists())
                if (auto font = AssetManager::GetDefaultFont()) lineH = font->GetLineHeight() * px / Font::kBakeSize;
            return { std::ceil(width + 1.0f), std::ceil(lineH * (float)std::max<size_t>(1, lines.Starts.size())) };
        }

        // What an element's content asks for (canvas units): a layout group's children, a text's lines (wrapping at
        // `wrapWidth` units), else its Size.
        glm::vec2 ContentSize(LayoutContext& ctx, Entity e, float wrapWidth)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            const auto& r = reg.Get<UIRectComponent>(e);
            if (auto* g = reg.TryGet<UILayoutGroupComponent>(e))
            {
                std::vector<glm::vec2> sizes;
                float inner = wrapWidth > 0.0f ? wrapWidth - g->Padding.x - g->Padding.z : -1.0f;
                for (Entity c : ctx.TheScene.GetChildren(e))
                    if (InLayout(reg, c)) sizes.push_back(ElementSize(ctx, c, g->StretchChildren ? inner : -1.0f));
                glm::vec2 pad(g->Padding.x + g->Padding.z, g->Padding.y + g->Padding.w);
                int n = (int)sizes.size();
                if (g->Direction == UILayoutDirection::Grid)
                {
                    int columns = wrapWidth > 0.0f ? GridColumns(*g, wrapWidth - pad.x) : (g->Columns > 0 ? g->Columns : std::max(1, n));
                    columns = std::max(1, std::min(columns, std::max(1, n)));
                    int rows = (n + columns - 1) / columns;
                    return pad + glm::vec2(columns * g->CellSize.x + (columns - 1) * g->Spacing, rows * g->CellSize.y + std::max(0, rows - 1) * g->Spacing);
                }
                if (g->Wrap && g->Direction == UILayoutDirection::Horizontal && wrapWidth > 0.0f)
                {
                    std::vector<glm::vec2> mins, rects;
                    FlowLines(ctx, e, *g, { wrapWidth - pad.x, 1e9f }, mins, rects);
                    float height = 0.0f;
                    for (size_t i = 0; i < mins.size(); ++i) height = std::max(height, mins[i].y + rects[i].y);
                    return { wrapWidth, height + pad.y };
                }
                int main = g->Direction == UILayoutDirection::Vertical ? 1 : 0;
                glm::vec2 total(0.0f);
                for (const glm::vec2& sz : sizes)
                {
                    total[main] += sz[main];
                    total[1 - main] = std::max(total[1 - main], sz[1 - main]);
                }
                if (n > 1) total[main] += g->Spacing * (n - 1);
                return total + pad;
            }
            if (auto* t = reg.TryGet<UITextComponent>(e)) return TextSize(*t, wrapWidth);
            return r.Size;
        }

        // The size a layout group gives a child (canvas units): its Size, its content's along axes its Content Size
        // Fitter fits, then its Layout Element's preferred and min sizes. `crossWidth`: the width it gets when the group
        // stretches it (text wraps there), else -1.
        glm::vec2 ElementSize(LayoutContext& ctx, Entity e, float crossWidth)
        {
            auto& reg = ctx.TheScene.GetRegistry();
            glm::vec2 size = reg.Get<UIRectComponent>(e).Size;
            if (auto* fit = reg.TryGet<UIContentSizeFitterComponent>(e))
            {
                bool fitX = fit->Horizontal == UIFitMode::PreferredSize, fitY = fit->Vertical == UIFitMode::PreferredSize;
                if (fitX || fitY)
                {
                    glm::vec2 content = ContentSize(ctx, e, fitX ? -1.0f : (crossWidth > 0.0f ? crossWidth : size.x));
                    if (fitX) size.x = content.x;
                    if (fitY) size.y = content.y;
                }
            }
            if (auto* le = reg.TryGet<UILayoutElementComponent>(e))
            {
                for (int i = 0; i < 2; ++i)
                {
                    if (le->PreferredSize[i] >= 0.0f) size[i] = le->PreferredSize[i];
                    if (le->MinSize[i] >= 0.0f) size[i] = std::max(size[i], le->MinSize[i]);
                }
            }
            return size;
        }

        size_t PrevCodepoint(const std::string& s, size_t i)
        {
            if (i == 0) return 0;
            i = std::min(i, s.size()) - 1;
            while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) --i;
            return i;
        }

        bool IsWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

        size_t WordLeft(const std::string& s, size_t i)
        {
            while (i > 0 && !IsWordChar(s[i - 1])) --i;
            while (i > 0 && IsWordChar(s[i - 1])) --i;
            return i;
        }

        size_t WordRight(const std::string& s, size_t i)
        {
            while (i < s.size() && !IsWordChar(s[i])) ++i;
            while (i < s.size() && IsWordChar(s[i])) ++i;
            return i;
        }

        // Caret position (byte offset) nearest to x, measured from the start of the text.
        size_t CaretFromX(const std::string& s, float x, float pixelSize)
        {
            size_t best = 0;
            float bestDist = std::abs(x);
            for (size_t i = NextCodepoint(s, 0); i <= s.size(); i = NextCodepoint(s, i))
            {
                float d = std::abs(MeasureText(s.substr(0, i), pixelSize) - x);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = i;
                }
                if (i == s.size()) break;
            }
            return best;
        }

        TextMeasure& MeasureFunction()
        {
            static TextMeasure measure;
            return measure;
        }

        constexpr glm::vec4 kDisabledTint{ 0.6f, 0.6f, 0.6f, 0.8f };
        constexpr glm::vec4 kPressedTint{ 0.85f, 0.85f, 0.85f, 1.0f };
    }

    void SetTextMeasure(TextMeasure measure) { MeasureFunction() = std::move(measure); }

    float MeasureText(const std::string& text, float pixelSize)
    {
        if (auto& custom = MeasureFunction()) return custom(text, pixelSize);
        // Without a GPU (headless tools, tests) the font cannot be loaded: approximate the width.
        if (!RenderDevice::Exists()) return 0.5f * pixelSize * (float)CodepointCount(text);
        auto font = AssetManager::GetDefaultFont();
        return font ? TextWidth(*font, text, pixelSize) : 0.0f;
    }

    size_t CodepointCount(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    namespace
    {
        bool IsSelectable(Registry& reg, Entity e)
        {
            if (!reg.Has<UIRectComponent>(e) || !reg.Get<TransformComponent>(e).ActiveInHierarchy) return false;
            const auto& r = reg.Get<UIRectComponent>(e);
            glm::vec2 mn = glm::max(r.ComputedMin, r.ClipMin), mx = glm::min(r.ComputedMax, r.ClipMax);
            if (mn.x >= mx.x || mn.y >= mx.y) return false; // scrolled or clipped out of view
            if (auto* b = reg.TryGet<UIButtonComponent>(e)) return b->Interactable;
            if (auto* t = reg.TryGet<UIToggleComponent>(e)) return t->Interactable;
            if (auto* s = reg.TryGet<UISliderComponent>(e)) return s->Interactable;
            if (auto* d = reg.TryGet<UIDropdownComponent>(e)) return d->Interactable;
            if (auto* f = reg.TryGet<UIInputFieldComponent>(e)) return f->Interactable;
            return false;
        }

        glm::vec2 CenterOf(Registry& reg, Entity e)
        {
            const auto& r = reg.Get<UIRectComponent>(e);
            return (r.ComputedMin + r.ComputedMax) * 0.5f;
        }

        // Widgets the arrows can reach, in draw order (UI Navigation mode None opts out).
        std::vector<Entity> NavigationCandidates(Registry& reg, const std::vector<Entity>& order)
        {
            std::vector<Entity> candidates;
            for (Entity e : order)
                if (IsSelectable(reg, e))
                    if (auto* nav = reg.TryGet<UINavigationComponent>(e); !nav || nav->Mode != UINavigationMode::None) candidates.push_back(e);
            return candidates;
        }

        // Where a move in `dir` goes from `selected` (null: the top-left widget): its explicit link, or in Automatic
        // mode the nearest widget that way. Returns null when it stays put. `isExplicit` reports a link was used.
        Entity NavigationTarget(Registry& reg, const std::vector<Entity>& candidates, Entity selected, glm::vec2 dir, bool* isExplicit = nullptr)
        {
            if (isExplicit) *isExplicit = false;
            if (candidates.empty()) return Entity::Null();
            const auto* nav = selected.IsNull() ? nullptr : reg.TryGet<UINavigationComponent>(selected);
            Entity link = Entity::Null();
            if (nav) link = dir.y < 0.0f ? nav->SelectOnUp : dir.y > 0.0f ? nav->SelectOnDown : dir.x < 0.0f ? nav->SelectOnLeft : nav->SelectOnRight;
            bool linkValid = !link.IsNull() && reg.Valid(link) && std::find(candidates.begin(), candidates.end(), link) != candidates.end();
            if (nav && nav->Mode == UINavigationMode::None) return Entity::Null();
            if (linkValid)
            {
                if (isExplicit) *isExplicit = true;
                return link;
            }
            if (nav && nav->Mode == UINavigationMode::Explicit) return Entity::Null(); // no link this way
            if (selected.IsNull())
            {
                // Nothing selected yet: start at the top-left widget.
                return *std::min_element(candidates.begin(), candidates.end(), [&](Entity a, Entity b) {
                    glm::vec2 ca = CenterOf(reg, a), cb = CenterOf(reg, b);
                    return ca.y + ca.x * 0.25f < cb.y + cb.x * 0.25f;
                });
            }
            glm::vec2 from = CenterOf(reg, selected);
            Entity best = Entity::Null();
            float bestScore = 1e30f;
            for (Entity c : candidates)
            {
                if (c == selected) continue;
                glm::vec2 d = CenterOf(reg, c) - from;
                float along = glm::dot(d, dir);
                if (along <= 1.0f) continue;
                float across = std::abs(d.x * dir.y - d.y * dir.x);
                float score = along + across * 2.0f; // prefer widgets straight ahead
                if (score < bestScore) { bestScore = score; best = c; }
            }
            return best;
        }

        // Keyboard / gamepad navigation (like Unity's EventSystem): the selection moves to the nearest widget in the
        // pressed direction, Submit presses it, sliders step with Left/Right, and an open dropdown list is navigated
        // with Up/Down. A mouse click selects the clicked widget (or clears the selection).
        void Navigate(Scene& scene, Registry& reg, const std::vector<Entity>& order, const PointerState& pointer, const glm::vec2& screen,
                      Entity hovered, std::vector<Entity>& clicked, std::vector<std::pair<Entity, float>>& values, Entity only)
        {
            Entity selected = scene.GetUISelected();
            // A selection on another canvas (the screen's while a World Space panel updates, or a panel's while the
            // screen does) is that canvas's to keep or clear; a click here still takes it.
            if (!selected.IsNull() && !pointer.Pressed && std::find(order.begin(), order.end(), selected) == order.end())
            {
                Entity owner = Entity::Null();
                for (Entity e = selected; scene.IsValid(e) && owner.IsNull(); e = scene.GetParent(e))
                    if (reg.Has<CanvasComponent>(e)) owner = e;
                if (!owner.IsNull() && reg.Get<TransformComponent>(owner).ActiveInHierarchy &&
                    (only.IsNull() ? reg.Get<CanvasComponent>(owner).RenderMode == CanvasRenderMode::WorldSpace : owner != only))
                    return;
            }
            std::vector<Entity> candidates = NavigationCandidates(reg, order);
            if (!selected.IsNull() && !IsSelectable(reg, selected)) selected = Entity::Null();
            if (pointer.Pressed)
            {
                selected = !hovered.IsNull() && IsSelectable(reg, hovered) ? hovered : Entity::Null();
                scene.SetUINavigating(false);
            }
            if (pointer.NavUp || pointer.NavDown || pointer.NavLeft || pointer.NavRight) scene.SetUINavigating(true);
            bool navigating = scene.IsUINavigating();

            bool typing = false;
            for (auto [e, f] : reg.ViewOf<UIInputFieldComponent>()) typing |= f.Focused;
            auto* field = selected.IsNull() ? nullptr : reg.TryGet<UIInputFieldComponent>(selected);
            if (typing)
            {
                // A focused text field keeps the arrows and Enter; Cancel (Escape, B) leaves it.
                if (pointer.Cancel && field && field->Focused) field->Focused = false;
                scene.SetUISelected(selected);
                return;
            }

            // An open dropdown list.
            if (auto* dd = selected.IsNull() || !navigating ? nullptr : reg.TryGet<UIDropdownComponent>(selected); dd && dd->Open)
            {
                int n = (int)dd->Options.size();
                if (pointer.NavUp || pointer.NavDown)
                {
                    int item = dd->HoveredItem >= 0 ? dd->HoveredItem : dd->Value;
                    dd->HoveredItem = std::clamp(item + (pointer.NavDown ? 1 : -1), 0, std::max(0, n - 1));
                    int visible = std::min(n, std::max(1, dd->MaxVisibleItems));
                    dd->ListScroll = std::clamp(dd->ListScroll, (float)(dd->HoveredItem - visible + 1), (float)dd->HoveredItem);
                }
                if (pointer.Submit && dd->HoveredItem >= 0 && dd->HoveredItem < n)
                {
                    if (dd->HoveredItem != dd->Value)
                    {
                        dd->Value = dd->HoveredItem;
                        values.emplace_back(selected, (float)dd->Value);
                    }
                    dd->Open = false;
                }
                if (pointer.Cancel) dd->Open = false;
                scene.SetUISelected(selected);
                return;
            }

            glm::vec2 dir(0.0f);
            if (pointer.NavLeft) dir = { -1, 0 };
            else if (pointer.NavRight) dir = { 1, 0 };
            else if (pointer.NavUp) dir = { 0, -1 };
            else if (pointer.NavDown) dir = { 0, 1 };
            auto* slider = selected.IsNull() ? nullptr : reg.TryGet<UISliderComponent>(selected);
            if (slider && dir.x != 0.0f)
            {
                float step = slider->WholeNumbers ? 1.0f : (slider->MaxValue - slider->MinValue) * 0.1f;
                float lo = std::min(slider->MinValue, slider->MaxValue), hi = std::max(slider->MinValue, slider->MaxValue);
                float v = std::clamp(slider->Value + step * dir.x, lo, hi);
                if (slider->WholeNumbers) v = std::round(v);
                if (v != slider->Value)
                {
                    slider->Value = v;
                    values.emplace_back(selected, v);
                }
            }
            else if (dir != glm::vec2(0.0f) && !candidates.empty())
            {
                if (Entity next = NavigationTarget(reg, candidates, selected, dir); !next.IsNull()) selected = next;
            }

            if (pointer.Submit && navigating && !selected.IsNull())
            {
                if (reg.Has<UIButtonComponent>(selected)) clicked.push_back(selected);
                else if (auto* toggle = reg.TryGet<UIToggleComponent>(selected))
                {
                    toggle->IsOn = !toggle->IsOn;
                    values.emplace_back(selected, toggle->IsOn ? 1.0f : 0.0f);
                }
                else if (auto* dd = reg.TryGet<UIDropdownComponent>(selected); dd && !dd->Options.empty())
                {
                    dd->Open = true;
                    dd->HoveredItem = dd->Value;
                }
                else if (field)
                {
                    field->Focused = true;
                    field->CaretTime = 0.0f;
                    field->Caret = (int32_t)field->Text.size();
                    field->SelectionAnchor = -1;
                }
            }
            (void)screen;
            scene.SetUISelected(selected);
        }
    }

    void DragRect(const UIRectComponent& rect, glm::ivec2 sides, glm::vec2 deltaPixels, glm::vec2& position, glm::vec2& size)
    {
        // min = ref + P*s - pivot*(span + S*s), max = min + span + S*s: moving one edge by d pixels changes S by +-d/s and
        // P by the pivot's share of it, so the other edge stays.
        glm::vec2 d = deltaPixels / std::max(rect.ComputedScale, 1e-4f);
        if (sides == glm::ivec2(0))
        {
            position += d;
            return;
        }
        for (int axis = 0; axis < 2; ++axis)
        {
            if (sides[axis] > 0)
            {
                size[axis] += d[axis];
                position[axis] += rect.Pivot[axis] * d[axis];
            }
            else if (sides[axis] < 0)
            {
                size[axis] -= d[axis];
                position[axis] += (1.0f - rect.Pivot[axis]) * d[axis];
            }
        }
    }

    Entity NavigationEntry(Scene& scene, Entity canvas, const glm::vec2& dir)
    {
        auto& reg = scene.GetRegistry();
        auto* c = reg.TryGet<CanvasComponent>(canvas);
        if (!c) return Entity::Null();
        std::vector<Entity> candidates = NavigationCandidates(reg, ComputeLayout(scene, c->PixelSize, canvas));
        Entity best = Entity::Null();
        float bestScore = 1e30f;
        for (Entity e : candidates)
        {
            glm::vec2 p = CenterOf(reg, e);
            float score = glm::dot(p, dir) * 4.0f + p.y + p.x * 0.25f; // the near side first, then top-left
            if (score < bestScore) { bestScore = score; best = e; }
        }
        return best;
    }

    std::vector<NavigationLink> NavigationLinks(Scene& scene, const std::vector<Entity>& order)
    {
        auto& reg = scene.GetRegistry();
        std::vector<Entity> candidates = NavigationCandidates(reg, order);
        static const glm::vec2 dirs[4] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
        std::vector<NavigationLink> links;
        for (Entity from : candidates)
            for (int d = 0; d < 4; ++d)
            {
                bool isExplicit = false;
                Entity to = NavigationTarget(reg, candidates, from, dirs[d], &isExplicit);
                if (to.IsNull() || to == from) continue;
                const auto& a = reg.Get<UIRectComponent>(from);
                const auto& b = reg.Get<UIRectComponent>(to);
                links.push_back({ from, to, (NavigationDirection)d, isExplicit, a.ComputedMin, a.ComputedMax, b.ComputedMin, b.ComputedMax });
            }
        return links;
    }

    namespace
    {
        struct CachedSheet
        {
            int64_t Version = -1;
            nlohmann::json Json;
        };

        // A style sheet's JSON and its version (the file's write time), read again when the file changes.
        const CachedSheet* LoadSheet(AssetHandle h)
        {
            static std::unordered_map<uint64_t, CachedSheet> cache;
            const AssetMetadata* meta = AssetManager::GetMetadata(h);
            if (!meta || meta->Type != AssetType::UIStyleSheet) return nullptr;
            int64_t version = (int64_t)meta->LastWrite.time_since_epoch().count();
            auto& c = cache[(uint64_t)h];
            if (c.Version != version)
            {
                c.Version = version;
                std::ifstream in(AssetManager::GetAbsolutePath(h));
                c.Json = nlohmann::json::parse(in, nullptr, false);
                if (c.Json.is_discarded() || !c.Json.is_object())
                {
                    Log::Warn("UI style sheet '{}' is not valid JSON", meta->Path.generic_string());
                    c.Json = nlohmann::json::object();
                }
            }
            return &c;
        }
    }

    namespace
    {
        enum StyleState : uint32_t { kHover = 1, kPressed = 2, kSelected = 4, kDisabled = 8 };
        const char* const kStateNames[] = { "hover", "pressed", "selected", "disabled" };

        // The pseudo-class states of an element (one frame behind: the last UI update's).
        uint32_t StyleStates(Scene& scene, Entity e)
        {
            auto& reg = scene.GetRegistry();
            uint32_t s = 0;
            for (Entity h = scene.GetUIHovered(); scene.IsValid(h); h = scene.GetParent(h))
                if (h == e) { s |= kHover; break; }
            if (auto* b = reg.TryGet<UIButtonComponent>(e))
            {
                if (b->Hovered) s |= kHover;
                if (b->Pressed) s |= kPressed;
                if (!b->Interactable) s |= kDisabled;
            }
            if (auto* t = reg.TryGet<UIToggleComponent>(e))
            {
                if (t->Pressed) s |= kPressed;
                if (!t->Interactable) s |= kDisabled;
            }
            if (auto* sl = reg.TryGet<UISliderComponent>(e))
            {
                if (sl->Dragging) s |= kPressed;
                if (!sl->Interactable) s |= kDisabled;
            }
            if (scene.GetUISelected() == e) s |= kSelected;
            return s;
        }

        nlohmann::json LerpJson(const nlohmann::json& a, const nlohmann::json& b, float k)
        {
            if (a.is_number() && b.is_number()) return a.get<double>() + (b.get<double>() - a.get<double>()) * k;
            if (a.is_array() && b.is_array() && a.size() == b.size())
            {
                nlohmann::json out = nlohmann::json::array();
                for (size_t i = 0; i < a.size(); ++i) out.push_back(LerpJson(a[i], b[i], k));
                return out;
            }
            return k < 1.0f ? a : b;
        }
    }

    int ApplyStyles(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        int restyled = 0;
        float now = scene.GetUnscaledTime();
        bool animate = scene.IsRunning();
        using json = nlohmann::json;
        for (auto [e, style] : reg.ViewOf<UIStyleComponent>())
        {
            auto field = [&](const std::string& component, const std::string& name, void*& data) -> const FieldInfo* {
                const ComponentInfo* info = ComponentRegistry::Find(component);
                if (!info || !info->Has(scene, e)) return nullptr;
                data = info->Get(scene, e);
                for (const FieldInfo& f : info->Fields)
                    if (f.Name == name) return &f;
                return nullptr;
            };
            auto set = [&](const std::string& component, const std::string& name, const json& value) {
                void* data = nullptr;
                if (const FieldInfo* f = field(component, name, data))
                {
                    try { FieldFromJson(*f, data, value, nullptr); }
                    catch (const std::exception&) { Log::Warn("UI style: bad value for {}.{}", component, name); }
                }
            };

            // The sheet: its own, else the nearest ancestor's.
            AssetHandle sheet = style.Sheet;
            for (Entity p = scene.GetParent(e); !sheet.IsValid() && scene.IsValid(p); p = scene.GetParent(p))
                if (auto* ps = reg.TryGet<UIStyleComponent>(p)) sheet = ps->Sheet;
            const CachedSheet* cached = sheet.IsValid() ? LoadSheet(sheet) : nullptr;
            int64_t version = cached ? cached->Version : -1;
            uint32_t states = StyleStates(scene, e);
            if (style.AppliedClasses != style.Classes || style.AppliedSheet != sheet || style.AppliedVersion != version ||
                style.AppliedStates != states)
            {
                style.AppliedClasses = style.Classes;
                style.AppliedSheet = sheet;
                style.AppliedVersion = version;
                style.AppliedStates = states;
                if (!style.Base.is_object()) style.Base = json::object();
                // What the styled fields show now (mid-transition values included), then back to the values from
                // before any rule (so a class removed or a state left takes its values with it).
                json before = json::object();
                for (auto& [component, fields] : style.Base.items())
                    for (auto& [name, value] : fields.items())
                    {
                        void* data = nullptr;
                        if (const FieldInfo* f = field(component, name, data)) before[component][name] = FieldToJson(*f, data, nullptr);
                        set(component, name, value);
                    }
                // The rules, in order: types, then classes; each followed by its active states' rules.
                std::vector<std::pair<const json*, std::string>> rules;
                if (cached)
                {
                    auto addWithStates = [&](const json& table, const std::string& name) {
                        if (auto it = table.find(name); it != table.end() && it->is_object()) rules.emplace_back(&*it, name);
                        for (int i = 0; i < 4; ++i)
                            if (states & (1u << i))
                                if (auto it = table.find(name + ":" + kStateNames[i]); it != table.end() && it->is_object())
                                    rules.emplace_back(&*it, name + ":" + kStateNames[i]);
                    };
                    static const json kNone = json::object();
                    auto tIt = cached->Json.find("types"), cIt = cached->Json.find("classes");
                    const json& types = tIt != cached->Json.end() && tIt->is_object() ? *tIt : kNone; // (rules point into the sheet)
                    for (const ComponentInfo& info : ComponentRegistry::All())
                        if (types.contains(info.Name) || types.contains(info.Name + ":hover") || types.contains(info.Name + ":pressed") ||
                            types.contains(info.Name + ":selected") || types.contains(info.Name + ":disabled"))
                            if (info.Has(scene, e)) addWithStates(types, info.Name);
                    const json& classes = cIt != cached->Json.end() && cIt->is_object() ? *cIt : kNone;
                    std::istringstream names(style.Classes);
                    for (std::string name; names >> name;) addWithStates(classes, name);
                }
                for (auto& [rule, label] : rules)
                {
                    float transition = rule->value("transition", 0.0f);
                    for (auto& [component, fields] : rule->items())
                    {
                        if (!fields.is_object()) continue;
                        for (auto& [name, value] : fields.items())
                        {
                            void* data = nullptr;
                            const FieldInfo* f = field(component, name, data);
                            if (!f) continue;
                            if (!style.Base[component].contains(name))
                            {
                                style.Base[component][name] = FieldToJson(*f, data, nullptr);
                                if (!before[component].contains(name)) before[component][name] = style.Base[component][name];
                            }
                            if (transition > 0.0f) style.Transitions[component][name] = transition;
                            set(component, name, value);
                        }
                    }
                }
                // Fields with a transition ease from what they showed to their new values.
                if (animate)
                    for (auto& [component, fields] : style.Base.items())
                        for (auto& [name, unused] : fields.items())
                        {
                            float seconds = style.Transitions.contains(component) ? style.Transitions[component].value(name, 0.0f) : 0.0f;
                            void* data = nullptr;
                            const FieldInfo* f = field(component, name, data);
                            if (seconds <= 0.0f || !f || !before.contains(component) || !before[component].contains(name)) continue;
                            json to = FieldToJson(*f, data, nullptr), from = before[component][name];
                            if (to == from) continue;
                            json kept = json::array();
                            for (auto& a : style.Anims)
                                if (a["component"] != component || a["field"] != name) kept.push_back(a);
                            style.Anims = kept;
                            style.Anims.push_back({ { "component", component }, { "field", name }, { "from", from }, { "to", to },
                                                    { "start", now }, { "seconds", seconds } });
                            set(component, name, from);
                        }
                ++restyled;
            }
            // Easing fields.
            if (style.Anims.is_array() && !style.Anims.empty())
            {
                json kept = json::array();
                for (auto& a : style.Anims)
                {
                    float k = std::clamp((now - a.value("start", 0.0f)) / std::max(a.value("seconds", 1.0f), 1e-4f), 0.0f, 1.0f);
                    if (!animate) k = 1.0f;
                    k = k * k * (3.0f - 2.0f * k); // ease in and out
                    set(a["component"].get<std::string>(), a["field"].get<std::string>(), LerpJson(a["from"], a["to"], k));
                    if (k < 1.0f) kept.push_back(a);
                }
                style.Anims = kept;
            }
        }
        return restyled;
    }

    namespace
    {
        struct BoundField
        {
            ScriptableEntity* Instance = nullptr;
            const FieldInfo* Field = nullptr;
            void* Object = nullptr;
            std::vector<std::string> Sub; // a path inside the field's value (object keys, array indices)
        };

        std::vector<std::string> SplitPath(const std::string& path)
        {
            std::vector<std::string> parts;
            for (size_t start = 0;;)
            {
                size_t dot = path.find('.', start);
                parts.push_back(path.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
            return parts;
        }

        // The script field a path names ("Script.Field.Sub...", the script by its full or short class name; or
        // "Field.Sub..." on the first script having that field): on `source`, or on `start` or its nearest parent.
        bool ResolvePath(Scene& scene, Entity start, Entity source, const std::string& path, BoundField& out)
        {
            auto& reg = scene.GetRegistry();
            if (path.empty()) return false;
            bool fixed = scene.IsValid(source);
            for (Entity s = fixed ? source : start; scene.IsValid(s); s = fixed ? Entity::Null() : scene.GetParent(s))
            {
                auto* sc = reg.TryGet<ScriptComponent>(s);
                if (!sc) continue;
                for (int pass = 0; pass < 2; ++pass) // the class named first, then a bare field
                    for (auto& inst : sc->Scripts)
                    {
                        if (!inst.Instance) continue;
                        std::string rest;
                        if (pass == 0)
                        {
                            std::string shortName = inst.ClassName.substr(inst.ClassName.find_last_of('.') + 1);
                            if (path.starts_with(inst.ClassName + ".")) rest = path.substr(inst.ClassName.size() + 1);
                            else if (path.starts_with(shortName + ".")) rest = path.substr(shortName.size() + 1);
                            else continue;
                        }
                        else
                            rest = path;
                        std::vector<std::string> parts = SplitPath(rest);
                        const ScriptInfo* info = ScriptRegistry::Find(inst.ClassName);
                        if (!info || parts.empty()) continue;
                        for (const FieldInfo& f : info->Fields)
                            if (f.Name == parts[0])
                            {
                                out = { inst.Instance.get(), &f, info->ToObject(inst.Instance.get()), { parts.begin() + 1, parts.end() } };
                                return true;
                            }
                    }
            }
            return false;
        }

        const nlohmann::json* JsonAt(const nlohmann::json& v, const std::vector<std::string>& sub)
        {
            const nlohmann::json* at = &v;
            for (const std::string& key : sub)
            {
                if (at->is_object() && at->contains(key)) at = &(*at)[key];
                else if (at->is_array() && !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit) && std::stoul(key) < at->size())
                    at = &(*at)[std::stoul(key)];
                else
                    return nullptr;
            }
            return at;
        }

        bool SetJsonAt(nlohmann::json& v, const std::vector<std::string>& sub, const nlohmann::json& value, size_t from = 0)
        {
            if (from == sub.size())
            {
                v = value;
                return true;
            }
            const std::string& key = sub[from];
            if (v.is_object() && v.contains(key)) return SetJsonAt(v[key], sub, value, from + 1);
            if (v.is_array() && !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit) && std::stoul(key) < v.size())
                return SetJsonAt(v[std::stoul(key)], sub, value, from + 1);
            return false;
        }

        nlohmann::json ReadBound(const BoundField& bf)
        {
            nlohmann::json full = FieldToJson(*bf.Field, bf.Object, nullptr);
            const nlohmann::json* at = JsonAt(full, bf.Sub);
            return at ? *at : nlohmann::json();
        }

        bool WriteBound(const BoundField& bf, const nlohmann::json& value)
        {
            bf.Instance->OnBeforeSerialize(); // the other fields as they are, before the managed object is updated
            nlohmann::json full = FieldToJson(*bf.Field, bf.Object, nullptr);
            if (!SetJsonAt(full, bf.Sub, value)) return false;
            try
            {
                FieldFromJson(*bf.Field, bf.Object, full, nullptr);
            }
            catch (const std::exception&)
            {
                return false;
            }
            bf.Instance->OnFieldsChanged(); // C#: pushed to the managed object
            return true;
        }

        // A list row's binding ("item", "item.Field", "index", "number" = index + 1): the row (this element or a
        // parent) and the path in it.
        bool RowPath(Scene& scene, Entity e, const std::string& path, Entity& row, std::vector<std::string>& sub, bool& index)
        {
            index = path == "index" || path == "number";
            if (!index && path != "item" && !path.starts_with("item.")) return false;
            for (row = e; scene.IsValid(row); row = scene.GetParent(row))
                if (scene.GetRegistry().Has<UIListRowComponent>(row)) break;
            if (!scene.IsValid(row)) return false;
            sub.clear();
            if (path.starts_with("item.")) sub = SplitPath(path.substr(5));
            return true;
        }

        // What a bindable widget shows (null: none).
        nlohmann::json WidgetValue(Registry& reg, Entity e)
        {
            if (auto* s = reg.TryGet<UISliderComponent>(e)) return s->Value;
            if (auto* t = reg.TryGet<UIToggleComponent>(e)) return t->IsOn;
            if (auto* f = reg.TryGet<UIInputFieldComponent>(e)) return f->Text;
            if (auto* d = reg.TryGet<UIDropdownComponent>(e)) return d->Value;
            return nullptr;
        }

        std::string BindingText(const nlohmann::json& v, const std::string& spec)
        {
            if (v.is_string()) return v.get<std::string>();
            if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
            if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
            if (v.is_number())
            {
                double d = v.get<double>();
                if (spec.size() >= 2 && (spec[0] == 'F' || spec[0] == 'f' || spec[0] == 'N' || spec[0] == 'n') && std::isdigit((unsigned char)spec[1]))
                    return std::format("{:.{}f}", d, std::clamp(std::atoi(spec.c_str() + 1), 0, 9));
                return std::format("{}", (float)d);
            }
            if (v.is_array())
            {
                std::string out;
                for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + BindingText(v[i], spec);
                return out;
            }
            return v.is_null() ? std::string() : v.dump();
        }
    }

    std::string FormatBinding(const nlohmann::json& value, const std::string& format)
    {
        if (format.empty()) return BindingText(value, "");
        std::string out = format;
        for (size_t at = out.find("{0"); at != std::string::npos; at = out.find("{0", at))
        {
            size_t close = out.find('}', at);
            if (close == std::string::npos) break;
            std::string spec = out[at + 2] == ':' ? out.substr(at + 3, close - at - 3) : std::string();
            std::string text = BindingText(value, spec);
            out.replace(at, close - at + 1, text);
            at += text.size();
        }
        return out;
    }

    void UpdateListViews(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        std::vector<Entity> lists;
        for (auto [e, lv] : reg.ViewOf<UIListViewComponent>()) lists.push_back(e);
        for (Entity e : lists)
        {
            if (!scene.IsValid(e) || !scene.IsActiveInHierarchy(e)) continue;
            auto* lv = reg.TryGet<UIListViewComponent>(e);
            if (!lv) continue;
            BoundField bf;
            nlohmann::json items = nlohmann::json::array();
            if (ResolvePath(scene, e, lv->Source, lv->Path, bf))
            {
                bf.Instance->OnBeforeSerialize();
                nlohmann::json v = ReadBound(bf);
                if (v.is_array()) items = std::move(v);
            }
            lv->Items = items;
            // The rows' parent: the view's scrolled content (its first UI child), made at first use.
            if (!scene.IsValid(lv->Content))
            {
                Entity content = scene.CreateEntity("List Rows", e);
                auto& r = reg.Emplace<UIRectComponent>(content);
                r.AnchorMin = { 0, 0 };
                r.AnchorMax = { 1, 0 };
                r.Pivot = { 0.5f, 0 };
                r.Size = { 0, 0 };
                scene.SetSiblingIndex(content, 0);
                lv = &reg.Get<UIListViewComponent>(e); // (pools may have moved)
                lv->Content = content;
            }
            float step = std::max(1.0f, lv->RowHeight + lv->Spacing);
            int n = (int)items.size();
            reg.Get<UIRectComponent>(lv->Content).Size.y = std::max(0.0f, n * step - lv->Spacing);
            // The rows in view (from the last layout: the view's height and scroll).
            float viewH = 0.0f, scroll = 0.0f;
            if (auto* r = reg.TryGet<UIRectComponent>(e)) viewH = (r->ComputedMax.y - r->ComputedMin.y) / std::max(r->ComputedScale, 1e-4f);
            if (auto* view = reg.TryGet<UIScrollViewComponent>(e)) scroll = view->ScrollPosition;
            if (viewH <= 0.0f) viewH = step * 8.0f;
            int first = std::clamp((int)std::floor(scroll / step), 0, std::max(0, n - 1));
            int count = std::clamp((int)std::ceil(viewH / step) + 1, 0, std::max(0, n - first));
            // Enough rows cloned from the template, the extra ones hidden.
            while ((int)lv->Rows.size() < count && scene.IsValid(lv->RowTemplate))
            {
                Entity row = scene.DuplicateEntity(lv->RowTemplate);
                scene.SetParent(row, lv->Content, false);
                reg.Emplace<UIListRowComponent>(row).List = e;
                lv = &reg.Get<UIListViewComponent>(e);
                lv->Rows.push_back(row);
            }
            for (size_t i = 0; i < lv->Rows.size(); ++i)
            {
                Entity row = lv->Rows[i];
                if (!scene.IsValid(row)) continue;
                bool shown = (int)i < count;
                if (reg.Get<TagComponent>(row).Active != shown) scene.SetActive(row, shown);
                if (!shown) continue;
                int index = first + (int)i;
                auto& rr = reg.Get<UIRectComponent>(row);
                rr.AnchorMin = { 0, 0 };
                rr.AnchorMax = { 1, 0 };
                rr.Pivot = { 0.5f, 0 };
                rr.Position = { 0, index * step };
                rr.Size = { 0, lv->RowHeight };
                auto& rc = reg.Get<UIListRowComponent>(row);
                rc.Index = index;
                rc.Item = items[index];
            }
        }
    }

    void PullBindings(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        std::unordered_set<ScriptableEntity*> pulled;
        for (auto [e, b] : reg.ViewOf<UIBindingComponent>())
        {
            nlohmann::json v;
            Entity row;
            std::vector<std::string> sub;
            bool index = false;
            if (RowPath(scene, e, b.Path, row, sub, index))
            {
                const auto& rc = reg.Get<UIListRowComponent>(row);
                if (index) v = rc.Index + (b.Path == "number" ? 1 : 0);
                else if (const nlohmann::json* at = JsonAt(rc.Item, sub)) v = *at;
            }
            else
            {
                BoundField bf;
                if (!ResolvePath(scene, e, b.Source, b.Path, bf)) continue;
                if (pulled.insert(bf.Instance).second) bf.Instance->OnBeforeSerialize(); // C#: the managed object's live values
                v = ReadBound(bf);
            }
            if (v == b.LastValue) continue;
            b.LastValue = v;
            if (auto* t = reg.TryGet<UITextComponent>(e)) t->Text = FormatBinding(v, b.Format);
            if (auto* s = reg.TryGet<UISliderComponent>(e); s && v.is_number()) s->Value = v.get<float>();
            if (auto* t = reg.TryGet<UIToggleComponent>(e); t && v.is_boolean()) t->IsOn = v.get<bool>();
            if (auto* f = reg.TryGet<UIInputFieldComponent>(e); f && v.is_string() && !f->Focused) f->Text = v.get<std::string>();
            if (auto* d = reg.TryGet<UIDropdownComponent>(e); d && v.is_number()) d->Value = v.get<int32_t>();
            b.LastWidget = WidgetValue(reg, e);
        }
    }

    void PushBindings(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        for (auto [e, b] : reg.ViewOf<UIBindingComponent>())
        {
            if (!b.TwoWay) continue;
            nlohmann::json w = WidgetValue(reg, e);
            if (w.is_null() || w == b.LastWidget) continue;
            b.LastWidget = w;
            Entity row;
            std::vector<std::string> sub;
            bool index = false;
            bool written = false;
            if (RowPath(scene, e, b.Path, row, sub, index))
            {
                // Into the list's item: the whole list field written back.
                auto& rc = reg.Get<UIListRowComponent>(row);
                auto* lv = reg.TryGet<UIListViewComponent>(rc.List);
                BoundField bf;
                if (!index && lv && ResolvePath(scene, rc.List, lv->Source, lv->Path, bf))
                {
                    sub.insert(sub.begin(), std::to_string(rc.Index));
                    bf.Sub.insert(bf.Sub.end(), sub.begin(), sub.end());
                    written = WriteBound(bf, w);
                    if (written) SetJsonAt(rc.Item, { sub.begin() + 1, sub.end() }, w);
                }
            }
            else
            {
                BoundField bf;
                if (!ResolvePath(scene, e, b.Source, b.Path, bf)) continue;
                written = WriteBound(bf, w);
            }
            if (!written) Log::Warn("UI Binding '{}': the widget's value could not be written", b.Path);
            b.LastValue = w;
        }
    }

    std::vector<Entity> ComputeLayout(Scene& scene, const glm::vec2& screen, Entity only)
    {
        auto& reg = scene.GetRegistry();
        ApplyStyles(scene);
        std::vector<std::pair<int32_t, Entity>> canvases;
        for (auto [e, canvas, t] : reg.ViewOf<CanvasComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy) continue;
            bool world = canvas.RenderMode == CanvasRenderMode::WorldSpace;
            if (only.IsNull() ? world : e != only) continue;
            canvases.emplace_back(canvas.SortingOrder, e);
        }
        std::stable_sort(canvases.begin(), canvases.end(), [](auto& a, auto& b) { return a.first < b.first; });

        std::vector<Entity> order;
        for (auto& [sortOrder, e] : canvases)
        {
            auto& canvas = reg.Get<CanvasComponent>(e);
            float scale = 1.0f;
            if (canvas.RenderMode == CanvasRenderMode::ScreenSpace && canvas.ScaleMode == CanvasScaleMode::ScaleWithScreenSize &&
                canvas.ReferenceResolution.x > 0 && canvas.ReferenceResolution.y > 0)
                scale = std::min(screen.x / canvas.ReferenceResolution.x, screen.y / canvas.ReferenceResolution.y);
            LayoutContext ctx{ scene, scale, order };
            LayoutNode(ctx, e, glm::vec2(0.0f), screen, glm::vec2(0.0f), screen, nullptr, nullptr);
        }
        return order;
    }

    std::vector<Entity> ElementsAt(Scene& scene, const glm::vec2& point, const glm::vec2& screen, const std::function<bool(Entity)>& alsoTakes, Entity canvas)
    {
        auto& reg = scene.GetRegistry();
        auto order = ComputeLayout(scene, screen, canvas);
        std::vector<Entity> under;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if ((BlocksPointer(reg, *it) || (alsoTakes && alsoTakes(*it))) && Contains(reg.Get<UIRectComponent>(*it), point)) under.push_back(*it);
        return under;
    }

    Entity Update(Scene& scene, const PointerState& pointer, const glm::vec2& screen, float dt, Entity only)
    {
        auto& reg = scene.GetRegistry();
        auto order = ComputeLayout(scene, screen, only);
        // Only this call's canvases' widgets (a World Space canvas is updated on its own, with its own pointer).
        std::vector<bool> inLayout;
        for (Entity e : order)
        {
            if (e.Index >= inLayout.size()) inLayout.resize(e.Index + 1, false);
            inLayout[e.Index] = true;
        }
        auto mine = [&](Entity e) { return e.Index < inLayout.size() && inLayout[e.Index]; };

        // Events are collected and dispatched after iteration: scripts may change the scene.
        std::vector<Entity> clicked;
        std::vector<std::pair<Entity, float>> values;
        std::vector<std::tuple<Entity, std::string, bool>> texts;

        // An open dropdown list is modal and a scrollbar drag owns the pointer: the other widgets see `input`
        // with the click and wheel removed.
        PointerState input = pointer;
        Entity captured = Entity::Null();
        for (auto [e, dd] : reg.ViewOf<UIDropdownComponent>())
        {
            if (!mine(e)) continue;
            if (!dd.Open) continue;
            auto* r = reg.TryGet<UIRectComponent>(e);
            if (!captured.IsNull() || !r || !reg.Get<TransformComponent>(e).ActiveInHierarchy || !dd.Interactable || dd.Options.empty())
            {
                dd.Open = false; // at most one list is open
                continue;
            }
            captured = e;
            int n = (int)dd.Options.size();
            DropdownList list = GetDropdownList(*r, dd, screen);
            float maxScroll = (float)(n - list.Visible);
            bool inList = InBox(pointer.Position, list.Min, list.Max);
            if (inList && pointer.Scroll != 0.0f) dd.ListScroll -= pointer.Scroll;
            dd.ListScroll = std::clamp(dd.ListScroll, 0.0f, maxScroll);
            // The mouse highlights the item under it; while navigating with keys/gamepad the highlight is theirs.
            if (inList) dd.HoveredItem = std::clamp((int)std::floor((pointer.Position.y - list.Min.y) / list.ItemHeight + dd.ListScroll), 0, n - 1);
            else if (!scene.IsUINavigating()) dd.HoveredItem = -1;
            if (pointer.Pressed)
            {
                // Picking an item selects it; a click anywhere else (including the box) just closes the list.
                if (dd.HoveredItem >= 0 && dd.HoveredItem != dd.Value)
                {
                    dd.Value = dd.HoveredItem;
                    values.emplace_back(e, (float)dd.Value);
                }
                dd.Open = false;
                dd.Pressed = false;
            }
            input.Pressed = input.Released = false;
            input.Scroll = 0.0f;
        }

        if (input.Pressed)
            for (auto it = order.rbegin(); it != order.rend() && input.Pressed; ++it)
            {
                auto* view = reg.TryGet<UIScrollViewComponent>(*it);
                auto& r = reg.Get<UIRectComponent>(*it);
                if (!view || !Contains(r, pointer.Position)) continue;
                for (bool horizontal : { false, true })
                {
                    ScrollbarGeometry g;
                    if (!GetScrollbar(r, *view, g, horizontal)) continue;
                    int axis = horizontal ? 0 : 1;
                    glm::vec2 grow(0.0f);
                    grow[1 - axis] = 3.0f * r.ComputedScale; // a slightly wider hit area
                    if (!InBox(pointer.Position, g.TrackMin - grow, g.TrackMax + grow)) continue;
                    // Grab the handle where it was clicked; clicking the track centres the handle on the pointer.
                    view->DraggingScrollbar = true;
                    view->PointerId = pointer.Id;
                    view->DraggingHorizontal = horizontal;
                    view->DragOffset = InBox(pointer.Position, g.HandleMin - grow, g.HandleMax + grow)
                                           ? pointer.Position[axis] - g.HandleMin[axis]
                                           : (g.HandleMax[axis] - g.HandleMin[axis]) * 0.5f;
                    input.Pressed = false;
                    break;
                }
            }
        for (auto [e, view] : reg.ViewOf<UIScrollViewComponent>())
        {
            if (!mine(e)) continue;
            if (view.PointerId != pointer.Id) continue; // another pointer's drag
            if (!pointer.Down) view.DraggingScrollbar = false;
            if (!view.DraggingScrollbar) continue;
            auto* r = reg.TryGet<UIRectComponent>(e);
            ScrollbarGeometry g;
            bool horizontal = view.DraggingHorizontal;
            if (!r || !GetScrollbar(*r, view, g, horizontal))
            {
                view.DraggingScrollbar = false;
                continue;
            }
            int axis = horizontal ? 0 : 1;
            float range = (g.TrackMax[axis] - g.TrackMin[axis]) - (g.HandleMax[axis] - g.HandleMin[axis]);
            float t = range > 0.0f ? std::clamp((pointer.Position[axis] - view.DragOffset - g.TrackMin[axis]) / range, 0.0f, 1.0f) : 0.0f;
            if (horizontal) view.ScrollPositionX = t * view.MaxScrollX;
            else view.ScrollPosition = t * view.MaxScroll;
            if (captured.IsNull()) captured = e;
            input.Pressed = input.Released = false;
        }

        // Topmost element under the pointer (last drawn wins); widgets and images block, text does not.
        Entity hovered = captured;
        if (hovered.IsNull())
            for (auto it = order.rbegin(); it != order.rend(); ++it)
                if (BlocksPointer(reg, *it) && Contains(reg.Get<UIRectComponent>(*it), pointer.Position))
                {
                    hovered = *it;
                    break;
                }

        // The wheel scrolls the innermost scroll view under the pointer (clamped by the next layout): vertically, or
        // sideways with Shift held or when the view only scrolls horizontally.
        if (input.Scroll != 0.0f)
            for (auto it = order.rbegin(); it != order.rend(); ++it)
                if (auto* view = reg.TryGet<UIScrollViewComponent>(*it); view && Contains(reg.Get<UIRectComponent>(*it), pointer.Position))
                {
                    bool sideways = view->Horizontal && (pointer.Shift || !view->Vertical);
                    if (sideways) view->ScrollPositionX = std::max(0.0f, view->ScrollPositionX - input.Scroll * view->ScrollSpeed);
                    else if (view->Vertical) view->ScrollPosition = std::max(0.0f, view->ScrollPosition - input.Scroll * view->ScrollSpeed);
                    break;
                }

        for (auto [e, dd] : reg.ViewOf<UIDropdownComponent>())
        {
            if (!mine(e)) continue;
            int n = (int)dd.Options.size();
            dd.Value = std::clamp(dd.Value, 0, std::max(0, n - 1));
            if (dd.Open) continue;
            bool over = e == hovered && dd.Interactable;
            if (input.Pressed && over) dd.Pressed = true;
            if (input.Released)
            {
                if (dd.Pressed && over && n > 0)
                {
                    dd.Open = true;
                    dd.HoveredItem = -1;
                    int visible = std::min(n, std::max(1, dd.MaxVisibleItems));
                    dd.ListScroll = (float)std::clamp(dd.Value - visible + 1, 0, n - visible); // keep the selection in view
                }
                dd.Pressed = false;
            }
        }

        for (auto [e, button] : reg.ViewOf<UIButtonComponent>())
        {
            if (!mine(e)) continue;
            bool over = e == hovered && button.Interactable;
            button.Hovered = over || (pointer.KeepHover && button.Hovered);
            // The tint fades to the state's color over Fade Duration (once a frame: not again for later pointers).
            if (!pointer.KeepHover)
            {
                bool navSelected = scene.IsUINavigating() && scene.GetUISelected() == e;
                glm::vec4 target = !button.Interactable ? kDisabledTint
                                 : button.Pressed ? button.PressedColor
                                 : button.Hovered || navSelected ? button.HoverColor
                                                                 : button.NormalColor;
                if (button.Tint.x < 0.0f) button.Tint = button.TintFrom = button.TintTarget = target;
                if (target != button.TintTarget)
                {
                    button.TintFrom = button.Tint;
                    button.TintTarget = target;
                    button.TintTime = 0.0f;
                }
                button.TintTime += dt;
                float k = button.FadeDuration > 0.0f ? std::clamp(button.TintTime / button.FadeDuration, 0.0f, 1.0f) : 1.0f;
                button.Tint = glm::mix(button.TintFrom, button.TintTarget, k);
            }
            if (input.Pressed && over)
            {
                button.Pressed = true;
                button.PointerId = pointer.Id;
            }
            if (input.Released && button.PointerId == pointer.Id)
            {
                if (button.Pressed && over) clicked.push_back(e);
                button.Pressed = false;
            }
        }

        for (auto [e, slider] : reg.ViewOf<UISliderComponent>())
        {
            if (!mine(e)) continue;
            if (input.Pressed && e == hovered && slider.Interactable)
            {
                slider.Dragging = true;
                slider.PointerId = pointer.Id;
            }
            if (slider.PointerId != pointer.Id) continue; // another pointer's drag
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
            if (!mine(e)) continue;
            bool over = e == hovered && toggle.Interactable;
            if (input.Pressed && over)
            {
                toggle.Pressed = true;
                toggle.PointerId = pointer.Id;
            }
            if (input.Released && toggle.PointerId == pointer.Id)
            {
                if (toggle.Pressed && over)
                {
                    toggle.IsOn = !toggle.IsOn;
                    values.emplace_back(e, toggle.IsOn ? 1.0f : 0.0f);
                }
                toggle.Pressed = false;
            }
        }

        // Input fields: caret, selection (keyboard and mouse drag), clipboard, Tab to the next field.
        std::vector<Entity> fieldOrder;
        for (Entity e : order)
            if (auto* f = reg.TryGet<UIInputFieldComponent>(e); f && f->Interactable) fieldOrder.push_back(e);
        Entity tabFrom = Entity::Null();
        bool tabBackwards = false;
        for (auto [e, field] : reg.ViewOf<UIInputFieldComponent>())
        {
            if (!mine(e)) continue;
            auto* rect = reg.TryGet<UIRectComponent>(e);
            if (!rect) continue;
            std::string& text = field.Text;
            float scale = rect->ComputedScale, pad = 10.0f * scale, px = field.FontSize * scale;
            float textLeft = rect->ComputedMin.x + pad;
            float lineH = px * 1.1f, textTop = rect->ComputedMin.y + pad * 0.5f;
            // Multi-line fields with Word Wrap break lines at the padded width (and never scroll sideways).
            float wrapWidth = field.MultiLine && field.WordWrap ? std::max(1.0f, rect->ComputedMax.x - rect->ComputedMin.x - 2.0f * pad) : 0.0f;
            // Caret offset under a point: the line under it (multi-line), then the column.
            auto caretAt = [&](glm::vec2 p) {
                if (!field.MultiLine) return (int32_t)CaretFromX(text, p.x - textLeft + field.ScrollX, px);
                TextLines lines = LayoutLines(text, wrapWidth, px);
                float row = std::floor((p.y - textTop + field.ScrollY) / lineH);
                size_t line = (size_t)std::clamp(row, 0.0f, (float)lines.Starts.size() - 1.0f);
                size_t a = lines.Starts[line], b = lines.Ends[line];
                size_t caret = a + CaretFromX(text.substr(a, b - a), p.x - textLeft + field.ScrollX, px);
                if (lines.Wrapped(line) && caret == b && b > a) caret = PrevCodepoint(text, b); // stay on this line
                return (int32_t)caret;
            };
            field.Caret = std::clamp(field.Caret, 0, (int32_t)text.size());
            if (field.SelectionAnchor > (int32_t)text.size()) field.SelectionAnchor = -1;

            if (input.Pressed)
            {
                if (e == hovered && field.Interactable)
                {
                    if (!field.Focused) field.CaretTime = 0.0f;
                    field.Focused = true;
                    field.Caret = caretAt(pointer.Position);
                    field.SelectionAnchor = field.Caret;
                    field.Selecting = true;
                }
                else
                    field.Focused = false;
            }
            if (field.Selecting)
            {
                if (pointer.Down) field.Caret = caretAt(pointer.Position);
                else field.Selecting = false;
            }
            if (!field.Focused)
            {
                field.SelectionAnchor = -1;
                field.Selecting = false;
                field.ScrollX = field.ScrollY = 0.0f;
                continue;
            }

            field.CaretTime += dt;
            std::string before = text;
            int32_t caretBefore = field.Caret, anchorBefore = field.SelectionAnchor;
            auto hasSelection = [&] { return field.SelectionAnchor >= 0 && field.SelectionAnchor != field.Caret; };
            auto selectionLo = [&] { return (size_t)std::min(field.Caret, field.SelectionAnchor); };
            auto selectionHi = [&] { return (size_t)std::max(field.Caret, field.SelectionAnchor); };
            auto eraseSelection = [&] {
                if (!hasSelection()) return false;
                size_t lo = selectionLo(), hi = selectionHi();
                text.erase(lo, hi - lo);
                field.Caret = (int32_t)lo;
                field.SelectionAnchor = -1;
                return true;
            };
            auto insert = [&](const std::string& typed) {
                eraseSelection();
                std::string add;
                size_t count = CodepointCount(text);
                for (size_t i = 0; i < typed.size();)
                {
                    unsigned char c = (unsigned char)typed[i];
                    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
                    bool printable = (c >= 32 && c != 127) || (c == '\n' && field.MultiLine); // drops tabs (and newlines in one line)
                    if (printable && (field.CharacterLimit <= 0 || count < (size_t)field.CharacterLimit))
                    {
                        add.append(typed, i, len);
                        ++count;
                    }
                    i += len;
                }
                text.insert((size_t)field.Caret, add);
                field.Caret += (int32_t)add.size();
                field.SelectionAnchor = -1;
            };
            auto moveCaret = [&](size_t to) {
                if (pointer.Shift)
                {
                    if (field.SelectionAnchor < 0) field.SelectionAnchor = field.Caret;
                }
                else
                    field.SelectionAnchor = -1;
                field.Caret = (int32_t)to;
            };

            if (pointer.SelectAll)
            {
                field.SelectionAnchor = 0;
                field.Caret = (int32_t)text.size();
            }
            if ((pointer.Copy || pointer.Cut) && hasSelection() && pointer.SetClipboard)
                pointer.SetClipboard(text.substr(selectionLo(), selectionHi() - selectionLo()));
            if (pointer.Cut) eraseSelection();
            if (pointer.Paste) insert(pointer.PasteText);
            if (!pointer.Text.empty()) insert(pointer.Text);
            if (pointer.Backspace && !eraseSelection())
            {
                size_t from = pointer.Ctrl ? WordLeft(text, field.Caret) : PrevCodepoint(text, field.Caret);
                text.erase(from, field.Caret - from);
                field.Caret = (int32_t)from;
            }
            if (pointer.Delete && !eraseSelection())
            {
                size_t to = pointer.Ctrl ? WordRight(text, field.Caret) : NextCodepoint(text, field.Caret);
                text.erase(field.Caret, to - field.Caret);
            }
            if (pointer.Left)
            {
                if (!pointer.Shift && hasSelection()) { field.Caret = (int32_t)selectionLo(); field.SelectionAnchor = -1; }
                else moveCaret(pointer.Ctrl ? WordLeft(text, field.Caret) : PrevCodepoint(text, field.Caret));
            }
            if (pointer.Right)
            {
                if (!pointer.Shift && hasSelection()) { field.Caret = (int32_t)selectionHi(); field.SelectionAnchor = -1; }
                else moveCaret(pointer.Ctrl ? WordRight(text, field.Caret) : NextCodepoint(text, field.Caret));
            }
            if (field.MultiLine && (pointer.LineUp || pointer.LineDown))
            {
                // Same x on the line above/below; past the first/last line, to the start/end.
                TextLines lines = LayoutLines(text, wrapWidth, px);
                size_t line = LineOf(lines.Starts, (size_t)field.Caret);
                float x = MeasureText(text.substr(lines.Starts[line], field.Caret - lines.Starts[line]), px);
                if (pointer.LineUp && line == 0) moveCaret(0);
                else if (pointer.LineDown && line + 1 >= lines.Starts.size()) moveCaret(text.size());
                else
                {
                    size_t to = pointer.LineUp ? line - 1 : line + 1, a = lines.Starts[to], b = lines.Ends[to];
                    size_t caret = a + CaretFromX(text.substr(a, b - a), x, px);
                    if (lines.Wrapped(to) && caret == b && b > a) caret = PrevCodepoint(text, b);
                    moveCaret(caret);
                }
            }
            if (field.MultiLine && !pointer.Ctrl && (pointer.Home || pointer.End))
            {
                TextLines lines = LayoutLines(text, wrapWidth, px);
                size_t line = LineOf(lines.Starts, (size_t)field.Caret);
                size_t end = lines.Ends[line];
                if (lines.Wrapped(line) && end > lines.Starts[line]) end = PrevCodepoint(text, end); // before the break
                moveCaret(pointer.Home ? lines.Starts[line] : end);
            }
            else
            {
                if (pointer.Home) moveCaret(0);
                if (pointer.End) moveCaret(text.size());
            }
            if (pointer.Tab)
            {
                tabFrom = e;
                tabBackwards = pointer.Shift;
            }

            bool newline = pointer.Enter && field.MultiLine && !pointer.Ctrl;
            if (newline) insert("\n");
            if (text != before || field.Caret != caretBefore || field.SelectionAnchor != anchorBefore) field.CaretTime = 0.0f;
            if (text != before) texts.emplace_back(e, text, false);
            if (pointer.Enter && !newline)
            {
                field.Focused = false;
                field.SelectionAnchor = -1;
                texts.emplace_back(e, text, true);
            }

            // Keep the caret inside the visible part of the field.
            float avail = std::max(1.0f, rect->ComputedMax.x - rect->ComputedMin.x - 2.0f * pad);
            float caretX, total;
            if (field.MultiLine)
            {
                TextLines lines = LayoutLines(text, wrapWidth, px);
                size_t line = LineOf(lines.Starts, (size_t)field.Caret);
                caretX = MeasureText(text.substr(lines.Starts[line], field.Caret - lines.Starts[line]), px);
                total = 0.0f;
                if (wrapWidth <= 0.0f)
                    for (size_t l = 0; l < lines.Starts.size(); ++l)
                        total = std::max(total, MeasureText(text.substr(lines.Starts[l], lines.Ends[l] - lines.Starts[l]), px));
                else
                    caretX = std::min(caretX, avail); // a hanging space never scrolls a wrapped field sideways
                float availY = std::max(lineH, rect->ComputedMax.y - rect->ComputedMin.y - pad);
                float top = line * lineH;
                if (top + lineH - field.ScrollY > availY) field.ScrollY = top + lineH - availY;
                if (top - field.ScrollY < 0.0f) field.ScrollY = top;
                field.ScrollY = std::clamp(field.ScrollY, 0.0f, std::max(0.0f, lines.Starts.size() * lineH - availY));
            }
            else
            {
                caretX = MeasureText(text.substr(0, field.Caret), px);
                total = MeasureText(text, px);
            }
            if (caretX - field.ScrollX > avail) field.ScrollX = caretX - avail;
            if (caretX - field.ScrollX < 0.0f) field.ScrollX = caretX;
            field.ScrollX = std::clamp(field.ScrollX, 0.0f, std::max(0.0f, total - avail));
        }
        if (!tabFrom.IsNull() && fieldOrder.size() > 1)
        {
            auto it = std::find(fieldOrder.begin(), fieldOrder.end(), tabFrom);
            if (it != fieldOrder.end())
            {
                size_t n = fieldOrder.size(), i = (size_t)std::distance(fieldOrder.begin(), it);
                Entity next = fieldOrder[tabBackwards ? (i + n - 1) % n : (i + 1) % n];
                auto& from = reg.Get<UIInputFieldComponent>(tabFrom);
                from.Focused = false;
                from.SelectionAnchor = -1;
                auto& to = reg.Get<UIInputFieldComponent>(next);
                to.Focused = true;
                to.CaretTime = 0.0f;
                to.SelectionAnchor = 0; // like Unity, tabbing in selects the text
                to.Caret = (int32_t)to.Text.size();
            }
        }

        Navigate(scene, reg, order, pointer, screen, hovered, clicked, values, only);

        for (Entity e : clicked) scene.DispatchUIClick(e);
        for (auto& [e, v] : values) scene.DispatchUIValueChanged(e, v);
        for (auto& [e, text, submit] : texts) scene.DispatchUIText(e, text, submit);
        return hovered;
    }

    bool WorldCanvasPoint(Scene& scene, Entity canvasEntity, const glm::vec2& screenPoint, const glm::vec2& screenSize, glm::vec2& pixel)
    {
        auto& reg = scene.GetRegistry();
        auto* canvas = reg.TryGet<CanvasComponent>(canvasEntity);
        Entity camera = scene.GetPrimaryCamera();
        if (!canvas || canvas->RenderMode != CanvasRenderMode::WorldSpace || camera.IsNull()) return false;
        glm::vec3 origin, direction;
        CameraProjection::ScreenPointToRay(scene, camera, screenPoint, screenSize, origin, direction);
        return WorldCanvasRayPoint(scene, canvasEntity, origin, direction, pixel);
    }

    bool WorldCanvasRayPoint(Scene& scene, Entity canvasEntity, const glm::vec3& origin, const glm::vec3& direction, glm::vec2& pixel)
    {
        auto& reg = scene.GetRegistry();
        auto* canvas = reg.TryGet<CanvasComponent>(canvasEntity);
        if (!canvas || canvas->RenderMode != CanvasRenderMode::WorldSpace) return false;
        glm::mat4 inv = glm::inverse(reg.Get<TransformComponent>(canvasEntity).World);
        glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f)), d = glm::vec3(inv * glm::vec4(direction, 0.0f));
        if (std::abs(d.z) < 1e-8f) return false;
        float t = -o.z / d.z;
        if (t <= 0.0f) return false;
        glm::vec3 hit = o + d * t; // the panel's plane (local z = 0), centered, +Y up
        glm::vec2 size = canvas->WorldSize();
        glm::vec2 f(hit.x / size.x + 0.5f, 0.5f - hit.y / size.y);
        pixel = f * canvas->PixelSize;
        return f.x >= 0.0f && f.y >= 0.0f && f.x <= 1.0f && f.y <= 1.0f;
    }

    void BuildDrawList(Scene& scene, const glm::vec2& screen, std::vector<DrawCommand>& out, Entity only)
    {
        auto& reg = scene.GetRegistry();
        auto order = ComputeLayout(scene, screen, only);
        uint32_t white = AssetManager::GetWhiteTexture()->GetBindlessIndex();
        Entity navSelected = scene.IsUINavigating() ? scene.GetUISelected() : Entity::Null();
        if (std::find(order.begin(), order.end(), navSelected) == order.end()) navSelected = Entity::Null(); // another canvas's

        // Each element's quads take the innermost UI Mask above it.
        size_t maskFrom = out.size();
        MaskInfo mask;
        auto finishMask = [&] {
            for (size_t i = maskFrom; i < out.size(); ++i) out[i].Mask = mask;
        };
        auto maskFor = [&](Entity e) {
            // Every mask above it with soft edges or an image shape (plain rects are clipped already), innermost first.
            MaskInfo m;
            for (Entity p = scene.GetParent(e); scene.IsValid(p) && m.Count < MaskInfo::kMaxLevels; p = scene.GetParent(p))
            {
                auto* mc = reg.TryGet<UIMaskComponent>(p);
                auto* mr = reg.TryGet<UIRectComponent>(p);
                if (!mc || !mr) continue;
                MaskLevel level;
                level.Rect = { mr->ComputedMin, mr->ComputedMax };
                level.Softness = glm::max(mc->Softness, glm::vec2(0.0f)) * mr->ComputedScale;
                if (mc->Shape == UIMaskShape::Image)
                    if (auto* img = reg.TryGet<UIImageComponent>(p))
                        if (auto tex = AssetManager::GetTexture(img->Texture)) level.Texture = tex->GetBindlessIndex();
                if (level.Softness != glm::vec2(0.0f) || level.Texture != 0) m.Levels[m.Count++] = level;
            }
            m.Active = m.Count > 0;
            return m;
        };

        for (Entity e : order)
        {
            finishMask();
            maskFrom = out.size();
            mask = maskFor(e);
            auto& rect = reg.Get<UIRectComponent>(e);
            glm::vec2 mn = rect.ComputedMin, mx = rect.ComputedMax, size = mx - mn;
            float s = rect.ComputedScale;

            auto* ownMask = reg.TryGet<UIMaskComponent>(e);
            if (auto* image = reg.TryGet<UIImageComponent>(e); image && (!ownMask || ownMask->ShowMaskGraphic))
            {
                glm::vec4 color = image->Color;
                if (auto* b = reg.TryGet<UIButtonComponent>(e))
                {
                    glm::vec4 tint = b->Tint.x >= 0.0f ? b->Tint // fading at runtime (UI::Update)
                                   : !b->Interactable ? kDisabledTint
                                   : b->Pressed       ? b->PressedColor
                                   : b->Hovered || e == navSelected ? b->HoverColor
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
                    const std::string& text = field->Text;
                    glm::vec2 origin{ mn.x + pad - field->ScrollX, mn.y };
                    size_t caret = std::min((size_t)std::max(field->Caret, 0), text.size());
                    float lineH = px * 1.1f, lineY = mn.y + (size.y - lineH) * 0.5f;
                    if (field->MultiLine)
                    {
                        // Lines from the top (scrolled), clipped to the padded field; selection per line.
                        inner.ClipMin.y = std::max(rect.ClipMin.y, mn.y + pad * 0.25f);
                        inner.ClipMax.y = std::min(rect.ClipMax.y, mx.y - pad * 0.25f);
                        float top = mn.y + pad * 0.5f - field->ScrollY;
                        bool placeholder = text.empty() && !field->Focused;
                        const std::string& shown = placeholder ? field->Placeholder : text;
                        float wrapWidth = field->WordWrap ? std::max(1.0f, size.x - 2.0f * pad) : 0.0f;
                        TextLines lines = LayoutLines(shown, wrapWidth, px);
                        bool selecting = field->Focused && field->SelectionAnchor >= 0 && field->SelectionAnchor != (int32_t)caret;
                        size_t lo = selecting ? std::min(caret, (size_t)field->SelectionAnchor) : 0, hi = selecting ? std::max(caret, std::min((size_t)field->SelectionAnchor, text.size())) : 0;
                        for (size_t l = 0; l < lines.Starts.size(); ++l)
                        {
                            size_t a = lines.Starts[l], b = lines.Ends[l];
                            float y = top + l * lineH;
                            if (y > mx.y || y + lineH < mn.y) continue;
                            if (selecting && hi > a && lo <= b)
                            {
                                float x0 = origin.x + MeasureText(shown.substr(a, std::max(lo, a) - a), px);
                                float x1 = origin.x + MeasureText(shown.substr(a, std::min(hi, b) - a), px) + (hi > b ? px * 0.3f : 0.0f);
                                Emit(out, { x0, y }, { x1, y + lineH }, { 0, 0 }, { 1, 1 }, { 0.25f, 0.55f, 0.95f, 0.35f }, white, inner);
                            }
                            if (b > a)
                                EmitText(out, *font, shown.substr(a, b - a), px, { origin.x, y }, lineH,
                                         placeholder ? field->PlaceholderColor : field->TextColor, inner);
                        }
                        if (field->Focused && std::fmod(field->CaretTime, 1.0f) < 0.55f)
                        {
                            TextLines cl = LayoutLines(text, wrapWidth, px);
                            size_t line = LineOf(cl.Starts, caret);
                            float cx = origin.x + MeasureText(text.substr(cl.Starts[line], caret - cl.Starts[line]), px), cy = top + line * lineH;
                            if (wrapWidth > 0.0f) cx = std::min(cx, origin.x + wrapWidth);
                            Emit(out, { cx, cy }, { cx + std::max(1.0f, 2.0f * s), cy + lineH }, { 0, 0 }, { 1, 1 }, field->TextColor, white, inner);
                        }
                    }
                    else if (field->Focused && field->SelectionAnchor >= 0 && field->SelectionAnchor != (int32_t)caret)
                    {
                        size_t lo = std::min(caret, (size_t)field->SelectionAnchor), hi = std::max(caret, std::min((size_t)field->SelectionAnchor, text.size()));
                        float x0 = origin.x + MeasureText(text.substr(0, lo), px), x1 = origin.x + MeasureText(text.substr(0, hi), px);
                        Emit(out, { x0, lineY }, { x1, lineY + lineH }, { 0, 0 }, { 1, 1 }, { 0.25f, 0.55f, 0.95f, 0.35f }, white, inner);
                    }
                    if (text.empty() && !field->Placeholder.empty() && !field->Focused)
                        EmitText(out, *font, field->Placeholder, px, origin, size.y, field->PlaceholderColor, inner);
                    else if (!text.empty())
                        EmitText(out, *font, text, px, origin, size.y, field->TextColor, inner);
                    if (field->Focused && std::fmod(field->CaretTime, 1.0f) < 0.55f)
                    {
                        float cx = origin.x + MeasureText(text.substr(0, caret), px);
                        Emit(out, { cx, lineY }, { cx + std::max(1.0f, 2.0f * s), lineY + lineH }, { 0, 0 }, { 1, 1 }, field->TextColor, white, inner);
                    }
                }
            }

            if (auto* dd = reg.TryGet<UIDropdownComponent>(e))
            {
                glm::vec4 dim = dd->Interactable ? glm::vec4(1.0f) : kDisabledTint;
                if (!reg.Has<UIImageComponent>(e))
                {
                    glm::vec4 bg = dd->BackgroundColor * dim;
                    if (dd->Pressed || dd->Open) bg *= kPressedTint;
                    Emit(out, mn, mx, { 0, 0 }, { 1, 1 }, bg, white, rect);
                }
                float pad = 10.0f * s, arrow = std::min(size.y * 0.4f, 18.0f * s);
                UIRectComponent inner = rect; // the label stops before the arrow
                inner.ClipMax.x = std::min(rect.ClipMax.x, mx.x - pad * 1.5f - arrow);
                if (auto font = AssetManager::GetDefaultFont(); font && dd->Value >= 0 && dd->Value < (int)dd->Options.size())
                    EmitText(out, *font, dd->Options[dd->Value], dd->FontSize * s, { mn.x + pad, mn.y }, size.y, dd->TextColor * dim, inner);
                EmitArrow(out, { mx.x - pad - arrow * 0.5f, mn.y + size.y * 0.5f }, arrow, dd->Open, dd->TextColor * dim, white, rect);
            }

            if (auto* text = reg.TryGet<UITextComponent>(e); text && (!text->Text.empty() || !text->LocalizedKey.empty()))
            {
                auto font = AssetManager::GetFont(text->Font);
                if (!font) continue;
                float pixelSize = text->FontSize * s;
                glm::vec2 block;
                std::string localized = text->LocalizedKey.empty() ? std::string() : Localization::Get(text->LocalizedKey);
                auto quads = font->Layout(text->LocalizedKey.empty() ? text->Text : localized, pixelSize, text->Wrap ? size.x : 0.0f, text->Align, size.x, block, text->RichText, s);
                float y = text->VerticalAlign == TextVAlign::Top      ? 0.0f
                        : text->VerticalAlign == TextVAlign::Middle   ? (size.y - block.y) * 0.5f
                                                                      : size.y - block.y;
                glm::vec2 origin = mn + glm::vec2(0.0f, y);
                EmitTextQuads(out, quads, origin, text->Color, font->GetAtlasIndex(), white, rect);
            }
        }

        finishMask();

        // Navigation highlight: an outline around the selected widget.
        if (!navSelected.IsNull() && reg.Has<UIRectComponent>(navSelected) && reg.Get<TransformComponent>(navSelected).ActiveInHierarchy)
        {
            const auto& rect = reg.Get<UIRectComponent>(navSelected);
            float w = std::max(2.0f, 3.0f * rect.ComputedScale), gap = 2.0f * rect.ComputedScale;
            glm::vec2 a = rect.ComputedMin - glm::vec2(gap + w), b = rect.ComputedMax + glm::vec2(gap + w);
            const glm::vec4 color{ 1.0f, 0.8f, 0.25f, 1.0f };
            UIRectComponent clip = rect;
            clip.ClipMin = glm::max(rect.ClipMin - glm::vec2(gap + w), glm::vec2(0.0f));
            clip.ClipMax = glm::min(rect.ClipMax + glm::vec2(gap + w), screen);
            Emit(out, a, { b.x, a.y + w }, { 0, 0 }, { 1, 1 }, color, white, clip);
            Emit(out, { a.x, b.y - w }, b, { 0, 0 }, { 1, 1 }, color, white, clip);
            Emit(out, { a.x, a.y + w }, { a.x + w, b.y - w }, { 0, 0 }, { 1, 1 }, color, white, clip);
            Emit(out, { b.x - w, a.y + w }, { b.x, b.y - w }, { 0, 0 }, { 1, 1 }, color, white, clip);
        }

        // A World Space panel: a dot where each script pointer ray meets it (the mouse has the system cursor).
        if (!only.IsNull())
        {
            UIRectComponent clip;
            clip.ClipMin = glm::vec2(0.0f);
            clip.ClipMax = screen;
            for (const glm::vec2& c : scene.UIPointerCursors(only))
            {
                Emit(out, c - glm::vec2(9.0f), c + glm::vec2(9.0f), { 0, 0 }, { 1, 1 }, { 0.05f, 0.05f, 0.08f, 0.85f }, white, clip);
                Emit(out, c - glm::vec2(6.0f), c + glm::vec2(6.0f), { 0, 0 }, { 1, 1 }, { 1.0f, 1.0f, 1.0f, 1.0f }, white, clip);
            }
        }

        // Scrollbars sit on top of their view's content.
        for (Entity e : order)
        {
            auto* view = reg.TryGet<UIScrollViewComponent>(e);
            if (!view) continue;
            const auto& rect = reg.Get<UIRectComponent>(e);
            for (bool horizontal : { false, true })
            {
                ScrollbarGeometry g;
                if (!GetScrollbar(rect, *view, g, horizontal)) continue;
                Emit(out, g.TrackMin, g.TrackMax, { 0, 0 }, { 1, 1 }, view->ScrollbarTrackColor, white, rect);
                glm::vec4 handle = view->ScrollbarHandleColor;
                if (view->DraggingScrollbar && view->DraggingHorizontal == horizontal) handle.a = std::min(1.0f, handle.a * 1.5f);
                Emit(out, g.HandleMin, g.HandleMax, { 0, 0 }, { 1, 1 }, handle, white, rect);
            }
        }

        // Open dropdown lists last, over everything and unclipped by their parents.
        UIRectComponent screenClip;
        screenClip.ClipMin = glm::vec2(0.0f);
        screenClip.ClipMax = screen;
        auto font = AssetManager::GetDefaultFont();
        for (Entity e : order)
        {
            auto* dd = reg.TryGet<UIDropdownComponent>(e);
            if (!dd || !dd->Open || dd->Options.empty()) continue;
            const auto& rect = reg.Get<UIRectComponent>(e);
            float s = rect.ComputedScale, pad = 10.0f * s;
            DropdownList list = GetDropdownList(rect, *dd, screen);
            Emit(out, list.Min + glm::vec2(4.0f * s), list.Max + glm::vec2(4.0f * s), { 0, 0 }, { 1, 1 }, { 0, 0, 0, 0.3f }, white, screenClip);
            Emit(out, list.Min, list.Max, { 0, 0 }, { 1, 1 }, dd->ListColor, white, screenClip);
            UIRectComponent listClip = screenClip;
            listClip.ClipMin = glm::max(list.Min, glm::vec2(0.0f));
            listClip.ClipMax = glm::min(list.Max, screen);
            int n = (int)dd->Options.size();
            int first = (int)std::floor(dd->ListScroll);
            for (int i = first; i < std::min(n, first + list.Visible + 1); ++i)
            {
                float y = list.Min.y + (i - dd->ListScroll) * list.ItemHeight;
                glm::vec2 a{ list.Min.x, y }, b{ list.Max.x, y + list.ItemHeight };
                bool hot = i == dd->HoveredItem;
                if (hot) Emit(out, a, b, { 0, 0 }, { 1, 1 }, dd->HighlightColor, white, listClip);
                else if (i == dd->Value) Emit(out, a, b, { 0, 0 }, { 1, 1 }, dd->HighlightColor * glm::vec4(1, 1, 1, 0.2f), white, listClip);
                if (i == dd->Value) // selection marker
                {
                    float m = list.ItemHeight * 0.18f;
                    Emit(out, { b.x - pad - m * 2.0f, y + list.ItemHeight * 0.5f - m }, { b.x - pad, y + list.ItemHeight * 0.5f + m }, { 0, 0 }, { 1, 1 },
                         hot ? glm::vec4(1.0f) : dd->HighlightColor, white, listClip);
                }
                if (font)
                    EmitText(out, *font, dd->Options[i], dd->FontSize * s, { a.x + pad, y }, list.ItemHeight, hot ? glm::vec4(1.0f) : dd->TextColor,
                             listClip);
            }
            if (n > list.Visible) // position indicator
            {
                float trackH = list.Max.y - list.Min.y, h = trackH * list.Visible / n;
                float y = list.Min.y + (trackH - h) * (dd->ListScroll / (float)(n - list.Visible));
                Emit(out, { list.Max.x - 6.0f * s, y }, { list.Max.x - 2.0f * s, y + h }, { 0, 0 }, { 1, 1 }, { 0, 0, 0, 0.35f }, white, listClip);
            }
        }
    }
}
