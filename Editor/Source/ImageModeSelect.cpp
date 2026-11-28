// Image Mode: selections (marquees, lassos, magic wand, Select menu), the Move tool, Free Transform, the Crop tool, the
// clipboard, the Edit / Image / Layer / Select menus and their dialogs.
#include "ImageMode.h"
#include "EditorUI.h"
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Imaging;

    namespace
    {
        ImVec2 V(const glm::vec2& p) { return ImVec2(p.x, p.y); }
        float Cross(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }
        // `d` in the basis (u, v).
        glm::vec2 Solve(const glm::vec2& u, const glm::vec2& v, const glm::vec2& d)
        {
            float det = Cross(u, v);
            if (std::abs(det) < 1e-9f) return glm::vec2(0.0f);
            return glm::vec2(Cross(d, v) / det, Cross(u, d) / det);
        }
        glm::vec2 Rotate(const glm::vec2& p, const glm::vec2& c, float angle)
        {
            float s = std::sin(angle), k = std::cos(angle);
            glm::vec2 d = p - c;
            return c + glm::vec2(d.x * k - d.y * s, d.x * s + d.y * k);
        }
        glm::mat3 Translation(glm::ivec2 by)
        {
            glm::mat3 m(1.0f);
            m[2] = glm::vec3((float)by.x, (float)by.y, 1.0f);
            return m;
        }
        void Dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, float phase)
        {
            // Marching ants: black under, white dashes over, moving with `phase`.
            dl->AddLine(a, b, IM_COL32(0, 0, 0, 255));
            const bool horizontal = std::abs(b.x - a.x) >= std::abs(b.y - a.y);
            float from = horizontal ? std::min(a.x, b.x) : std::min(a.y, b.y), to = horizontal ? std::max(a.x, b.x) : std::max(a.y, b.y);
            float start = std::floor((from + phase) / 8.0f) * 8.0f - phase;
            for (float t = start; t < to; t += 8.0f)
            {
                float t0 = std::max(t, from), t1 = std::min(t + 4.0f, to);
                if (t1 <= t0) continue;
                dl->AddLine(horizontal ? ImVec2(t0, a.y) : ImVec2(a.x, t0), horizontal ? ImVec2(t1, a.y) : ImVec2(a.x, t1), IM_COL32(255, 255, 255, 255));
            }
        }
    }

    bool ImageMode::IsSelectTool(Tool tool)
    {
        return tool == Tool::RectMarquee || tool == Tool::EllipseMarquee || tool == Tool::Lasso || tool == Tool::PolyLasso || tool == Tool::MagicWand;
    }

    // ---- Selections

    SelectOp ImageMode::OpFromKeys() const
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && io.KeyAlt) return SelectOp::Intersect;
        if (io.KeyShift) return SelectOp::Add;
        if (io.KeyAlt) return SelectOp::Subtract;
        return m_SelectOp;
    }

    void ImageMode::SetSelection(Mask shape, SelectOp op, const char* label)
    {
        Mask result = std::move(shape);
        if (op != SelectOp::Replace)
        {
            Mask base = m_Doc.Selection ? *m_Doc.Selection : Mask::Solid(m_Doc.Width, m_Doc.Height, 0);
            Combine(base, result, op);
            result = std::move(base);
        }
        Record(label);
        if (m_Doc.Selection) m_LastSelection = m_Doc.Selection;
        if (IsEmpty(result)) m_Doc.Selection.reset();
        else m_Doc.Selection = std::make_shared<Mask>(std::move(result));
        Changed(false);
    }

    void ImageMode::Deselect()
    {
        if (!m_Doc.Selection) return;
        Record("Deselect");
        m_LastSelection = m_Doc.Selection;
        m_Doc.Selection.reset();
        Changed(false);
    }

    void ImageMode::SelectAll() { SetSelection(Mask::Solid(m_Doc.Width, m_Doc.Height, 255), SelectOp::Replace, "Select All"); }

    void ImageMode::Reselect()
    {
        if (!m_LastSelection || m_LastSelection->Width != m_Doc.Width || m_LastSelection->Height != m_Doc.Height) return;
        Record("Reselect");
        m_Doc.Selection = m_LastSelection;
        Changed(false);
    }

    void ImageMode::InverseSelection()
    {
        if (!m_Doc.Selection) return;
        Mask inv = *m_Doc.Selection;
        InvertSelection(inv);
        SetSelection(std::move(inv), SelectOp::Replace, "Select Inverse");
    }

    void ImageMode::ModifySelection(int kind, float amount)
    {
        if (!m_Doc.Selection) return;
        Mask m = *m_Doc.Selection;
        const char* label = "Modify Selection";
        switch (kind)
        {
        case 4: Expand(m, (int)std::lround(amount)), label = "Expand"; break;
        case 5: Expand(m, -(int)std::lround(amount)), label = "Contract"; break;
        case 6: Feather(m, amount), label = "Feather"; break;
        case 7: m = Border(m, std::max(1, (int)std::lround(amount))), label = "Border"; break;
        default: return;
        }
        SetSelection(std::move(m), SelectOp::Replace, label);
    }

    void ImageMode::SelectionInput(const glm::vec2& p, bool hovered, Tool tool)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool noKeys = !io.KeyShift && !io.KeyAlt && !io.KeyCtrl;
        auto insideSelection = [&] {
            int x = (int)std::floor(p.x), y = (int)std::floor(p.y);
            return m_Doc.Selection && x >= 0 && y >= 0 && x < (int)m_Doc.Width && y < (int)m_Doc.Height && m_Doc.Selection->At((uint32_t)x, (uint32_t)y) >= 128;
        };

        // Dragging pixels (the Move tool) or the selection's outline (a selection tool inside the selection).
        if (m_Move.Active)
        {
            if (down) UpdateMove(glm::ivec2(glm::round(p - m_Move.Start)));
            else EndMove();
            return;
        }
        if (tool == Tool::Move)
        {
            if (clicked) BeginMove(p, false);
            return;
        }
        if (clicked && tool != Tool::MagicWand && tool != Tool::PolyLasso && noKeys && insideSelection())
        {
            BeginMove(p, true);
            return;
        }

        switch (tool)
        {
        case Tool::RectMarquee:
        case Tool::EllipseMarquee:
            if (clicked)
            {
                m_SelDrag = true;
                m_SelStart = m_SelEnd = p;
                m_DragOp = OpFromKeys();
                m_DragMods = (io.KeyShift ? 1 : 0) | (io.KeyAlt ? 2 : 0);
            }
            if (m_SelDrag)
            {
                m_SelEnd = p;
                if (!down)
                {
                    m_SelDrag = false;
                    // Shift pressed during the drag: a square (circle); Alt: from the center.
                    glm::vec2 a = m_SelStart, b = m_SelEnd;
                    if (io.KeyShift && !(m_DragMods & 1))
                    {
                        float side = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                        b = a + glm::vec2(b.x >= a.x ? side : -side, b.y >= a.y ? side : -side);
                    }
                    if (io.KeyAlt && !(m_DragMods & 2)) a = a - (b - a);
                    if (std::abs(b.x - a.x) < 1.0f && std::abs(b.y - a.y) < 1.0f)
                    {
                        if (m_DragOp == SelectOp::Replace) Deselect(); // (a click without a drag)
                        break;
                    }
                    Mask shape = SelectRect(m_Doc.Width, m_Doc.Height, a, b, tool == Tool::EllipseMarquee, m_SelAntialias || tool == Tool::EllipseMarquee);
                    if (m_SelFeather > 0.0f) Feather(shape, m_SelFeather);
                    SetSelection(std::move(shape), m_DragOp, tool == Tool::EllipseMarquee ? "Elliptical Marquee" : "Rectangular Marquee");
                }
            }
            break;
        case Tool::Lasso:
            if (clicked)
            {
                m_LassoActive = true;
                m_Lasso = { p };
                m_DragOp = OpFromKeys();
            }
            if (m_LassoActive)
            {
                if (down)
                {
                    if (glm::length(p - m_Lasso.back()) * m_Zoom >= 2.0f) m_Lasso.push_back(p);
                }
                else
                {
                    m_LassoActive = false;
                    if (m_Lasso.size() >= 3)
                    {
                        Mask shape = SelectPolygon(m_Doc.Width, m_Doc.Height, m_Lasso, m_SelAntialias);
                        if (m_SelFeather > 0.0f) Feather(shape, m_SelFeather);
                        SetSelection(std::move(shape), m_DragOp, "Lasso");
                    }
                    else if (m_DragOp == SelectOp::Replace)
                        Deselect();
                    m_Lasso.clear();
                }
            }
            break;
        case Tool::PolyLasso:
            if (clicked)
            {
                if (!m_LassoActive)
                {
                    m_LassoActive = true;
                    m_Lasso = { p };
                    m_DragOp = OpFromKeys();
                }
                else
                {
                    // Closed by clicking the first point (or a double-click; Enter also closes it).
                    bool closing = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || glm::length(p - m_Lasso.front()) * m_Zoom < 8.0f;
                    if (!closing) m_Lasso.push_back(p);
                    else
                    {
                        m_LassoActive = false;
                        if (m_Lasso.size() >= 3)
                        {
                            Mask shape = SelectPolygon(m_Doc.Width, m_Doc.Height, m_Lasso, m_SelAntialias);
                            if (m_SelFeather > 0.0f) Feather(shape, m_SelFeather);
                            SetSelection(std::move(shape), m_DragOp, "Polygonal Lasso");
                        }
                        m_Lasso.clear();
                    }
                }
            }
            break;
        case Tool::MagicWand:
            if (clicked && p.x >= 0 && p.y >= 0 && p.x < m_Doc.Width && p.y < m_Doc.Height)
            {
                const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
                const Bitmap& source = m_WandSampleAll || !l.IsRaster() || !l.Pixels ? m_Composite : l.Get();
                if (source.Width == m_Doc.Width)
                    SetSelection(SelectColor(source, glm::ivec2(p), m_WandTolerance, m_WandContiguous), OpFromKeys(), "Magic Wand");
            }
            break;
        default: break;
        }
    }

    // ---- Moving

    std::vector<int> ImageMode::AffectedLayers()
    {
        std::vector<int> out;
        if (m_Doc.Layers.empty()) return out;
        const Layer& active = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (active.IsGroup())
        {
            for (size_t i = 0; i < m_Doc.Layers.size(); ++i)
                if ((m_Doc.Layers[i].HasPixels() || m_Doc.Layers[i].IsVector()) && m_Doc.IsInside((int)i, active.Id)) out.push_back((int)i);
        }
        else if (active.HasPixels() || active.IsVector())
            out.push_back(m_ActiveLayer);
        else
        {
            Note("An adjustment layer has no pixels to move");
            return out;
        }
        for (int i : out)
            if (m_Doc.Layers[(size_t)i].LockAll || m_Doc.Layers[(size_t)i].LockPixels)
            {
                Note("A layer is locked: '" + m_Doc.Layers[(size_t)i].Name + "'");
                return {};
            }
        return out;
    }

    bool ImageMode::BeginMove(const glm::vec2& p, bool selectionOnly)
    {
        m_Move = MoveState();
        if (!selectionOnly)
        {
            std::vector<int> layers = AffectedLayers();
            if (layers.empty()) return false;
            for (int i : layers)
            {
                const Layer& l = m_Doc.Layers[(size_t)i];
                m_Move.Layers.push_back({ l.Id, l.Pixels, l.LayerMask, l.Text, l.Shape });
            }
        }
        else if (!m_Doc.Selection)
            return false;
        Record(selectionOnly ? "Move Selection" : "Move");
        m_Move.Active = true;
        m_Move.SelectionOnly = selectionOnly;
        m_Move.Selection = m_Doc.Selection;
        m_Move.Start = p;
        return true;
    }

    void ImageMode::UpdateMove(glm::ivec2 offset)
    {
        if (!m_Move.Active || offset == m_Move.Offset) return;
        m_Move.Offset = offset;
        const glm::mat3 t = Translation(offset);
        const Mask* sel = m_Move.Selection.get();
        for (const LayerCopy& c : m_Move.Layers)
        {
            int i = m_Doc.IndexOf(c.Id);
            if (i < 0) continue;
            Layer& l = m_Doc.Layers[(size_t)i];
            // Text and shapes move by their settings (whole: a selection does not cut them).
            if (l.Kind == LayerKind::Shape) l.Shape.Center = c.Shape.Center + glm::vec2(offset);
            else if (l.Kind == LayerKind::Text) l.Text.Position = c.Text.Position + glm::vec2(offset);
            else l.Pixels = std::make_shared<Bitmap>(TransformPixels(*c.Pixels, t, sel, Resample::Nearest));
            if ((!sel || l.IsVector()) && c.LayerMask) l.LayerMask = std::make_shared<Mask>(TransformMask(*c.LayerMask, t, 255, Resample::Nearest));
        }
        if (sel) m_Doc.Selection = std::make_shared<Mask>(TransformMask(*sel, t, 0, Resample::Nearest));
        MarkAll();
    }

    void ImageMode::EndMove()
    {
        if (!m_Move.Active) return;
        m_Move.Active = false;
        if (m_Move.Offset == glm::ivec2(0) && !m_Undo.empty()) m_Undo.pop_back(); // (nothing moved)
        else Changed();
        m_Move = MoveState();
    }

    void ImageMode::Nudge(glm::ivec2 by)
    {
        bool selectionOnly = m_Tool != Tool::Move;
        if (!BeginMove(glm::vec2(0.0f), selectionOnly)) return;
        UpdateMove(by);
        EndMove();
    }

    // ---- Free Transform

    bool ImageMode::BeginTransform()
    {
        if (m_Transform.Active) return true;
        std::vector<int> layers = AffectedLayers();
        if (layers.empty()) return false;
        Rect bounds;
        if (m_Doc.Selection) bounds = Bounds(*m_Doc.Selection);
        else
            for (int i : layers)
                if (m_Doc.Layers[(size_t)i].Pixels) bounds.Add(ContentBounds(m_Doc.Layers[(size_t)i].Get()));
        if (bounds.Empty())
        {
            Note("Nothing to transform here");
            return false;
        }
        Record("Free Transform");
        m_Transform = TransformState();
        m_Transform.Active = true;
        for (int i : layers)
        {
            const Layer& l = m_Doc.Layers[(size_t)i];
            m_Transform.Layers.push_back({ l.Id, l.Pixels, l.LayerMask, l.Text, l.Shape });
        }
        m_Transform.Selection = m_Doc.Selection;
        const glm::vec2 a((float)bounds.X0, (float)bounds.Y0), b((float)bounds.X1 + 1.0f, (float)bounds.Y1 + 1.0f);
        m_Transform.Source[0] = a;
        m_Transform.Source[1] = { b.x, a.y };
        m_Transform.Source[2] = b;
        m_Transform.Source[3] = { a.x, b.y };
        // One text or shape layer: the box is its own (turned with it).
        if (layers.size() == 1 && !m_Doc.Selection && m_Doc.Layers[(size_t)layers[0]].IsVector())
        {
            const Layer& l = m_Doc.Layers[(size_t)layers[0]];
            std::array<glm::vec2, 4> c = l.Kind == LayerKind::Shape ? ShapeCorners(l.Shape) : TextCorners(l.Text, LayoutText(l.Text));
            std::copy(c.begin(), c.end(), m_Transform.Source);
        }
        std::copy(m_Transform.Source, m_Transform.Source + 4, m_Transform.Quad);
        return true;
    }

    void ImageMode::ApplyTransform()
    {
        if (!m_Transform.Active) return;
        m_Transform.Dirty = false;
        const glm::mat3 h = Homography(m_Transform.Source, m_Transform.Quad);
        const Mask* sel = m_Transform.Selection.get();
        for (const LayerCopy& c : m_Transform.Layers)
        {
            int i = m_Doc.IndexOf(c.Id);
            if (i < 0) continue;
            Layer& l = m_Doc.Layers[(size_t)i];
            if (l.IsVector())
            {
                // Text and shapes take the map's move, turn and scale at their anchor (skew and perspective: not).
                auto mapped = [&](glm::vec2 anchor, float rotation, float& turned, glm::vec2& scale) {
                    glm::vec2 at = Apply(h, anchor);
                    float a = glm::radians(rotation);
                    glm::vec2 ex = Apply(h, anchor + glm::vec2(std::cos(a), std::sin(a))) - at;
                    glm::vec2 ey = Apply(h, anchor + glm::vec2(-std::sin(a), std::cos(a))) - at;
                    turned = glm::degrees(std::atan2(ex.y, ex.x));
                    scale = glm::vec2(glm::length(ex), glm::length(ey));
                    return at;
                };
                glm::vec2 scale(1.0f);
                if (l.Kind == LayerKind::Shape)
                {
                    l.Shape.Center = mapped(c.Shape.Center, c.Shape.Rotation, l.Shape.Rotation, scale);
                    l.Shape.Size = c.Shape.Size * scale;
                }
                else
                {
                    l.Text.Position = mapped(c.Text.Position, c.Text.Rotation, l.Text.Rotation, scale);
                    l.Text.Size = std::max(1.0f, c.Text.Size * scale.y);
                    l.Text.Tracking = c.Text.Tracking * scale.x;
                }
            }
            else
                l.Pixels = std::make_shared<Bitmap>(TransformPixels(*c.Pixels, h, sel, m_Transform.Method));
            if ((!sel || l.IsVector()) && c.LayerMask) l.LayerMask = std::make_shared<Mask>(TransformMask(*c.LayerMask, h, 255, m_Transform.Method));
        }
        if (sel) m_Doc.Selection = std::make_shared<Mask>(TransformMask(*sel, h, 0, m_Transform.Method));
        MarkAll();
    }

    void ImageMode::CommitTransform()
    {
        if (!m_Transform.Active) return;
        bool moved = false;
        for (int k = 0; k < 4; ++k) moved |= glm::length(m_Transform.Quad[k] - m_Transform.Source[k]) > 1e-3f;
        if (m_Transform.Dirty) ApplyTransform();
        m_Transform = TransformState();
        if (!moved && !m_Undo.empty()) m_Undo.pop_back();
        else Changed();
    }

    void ImageMode::CancelTransform()
    {
        if (!m_Transform.Active) return;
        for (const LayerCopy& c : m_Transform.Layers)
            if (int i = m_Doc.IndexOf(c.Id); i >= 0)
            {
                m_Doc.Layers[(size_t)i].Pixels = c.Pixels;
                m_Doc.Layers[(size_t)i].LayerMask = c.LayerMask;
                m_Doc.Layers[(size_t)i].Text = c.Text;
                m_Doc.Layers[(size_t)i].Shape = c.Shape;
            }
        m_Doc.Selection = m_Transform.Selection;
        m_Transform = TransformState();
        if (!m_Undo.empty()) m_Undo.pop_back();
        MarkAll();
    }

    void ImageMode::TransformInput(const glm::vec2& p, bool hovered)
    {
        TransformState& t = m_Transform;
        const ImGuiIO& io = ImGui::GetIO();
        auto screen = [&](const glm::vec2& q) { return glm::vec2(m_CanvasMin.x + q.x * m_Zoom, m_CanvasMin.y + q.y * m_Zoom); };
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        auto hit = [&]() -> int {
            for (int k = 0; k < 4; ++k)
                if (glm::length(screen(t.Quad[k]) - mouse) < 8.0f) return k;
            for (int k = 0; k < 4; ++k)
                if (glm::length(screen((t.Quad[k] + t.Quad[(k + 1) % 4]) * 0.5f) - mouse) < 8.0f) return 4 + k;
            // Inside the (convex) quad: move; outside: rotate.
            bool inside = true;
            for (int k = 0; k < 4; ++k)
                inside &= Cross(t.Quad[(k + 1) % 4] - t.Quad[k], p - t.Quad[k]) >= 0.0f;
            bool insideReversed = true;
            for (int k = 0; k < 4; ++k)
                insideReversed &= Cross(t.Quad[(k + 1) % 4] - t.Quad[k], p - t.Quad[k]) <= 0.0f;
            return inside || insideReversed ? 8 : 9;
        };
        if (hovered && t.Drag < 0)
        {
            int h = hit();
            ImGui::SetMouseCursor(h == 8 ? ImGuiMouseCursor_ResizeAll : h == 9 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeNESW);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && h == 8)
            {
                CommitTransform();
                return;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                t.Drag = h;
                t.DragStart = p;
                std::copy(t.Quad, t.Quad + 4, t.StartQuad);
            }
        }
        if (t.Drag < 0) return;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            t.Drag = -1;
            return;
        }
        const glm::vec2* S = t.StartQuad;
        glm::vec2 Q[4];
        std::copy(S, S + 4, Q);
        const glm::vec2 center = (S[0] + S[1] + S[2] + S[3]) * 0.25f;
        const glm::vec2 delta = p - t.DragStart;
        const glm::vec2 u = S[1] - S[0], v = S[3] - S[0];
        // Every corner scaled in the box's own axes (su along the top, sv down the side) about `o`.
        auto scaleAbout = [&](const glm::vec2& o, float su, float sv) {
            for (int k = 0; k < 4; ++k)
            {
                glm::vec2 ab = Solve(u, v, S[k] - o);
                Q[k] = o + u * (ab.x * su) + v * (ab.y * sv);
            }
        };
        if (t.Drag == 8)
            for (glm::vec2& q : Q) q += delta;
        else if (t.Drag == 9)
        {
            float angle = std::atan2(p.y - center.y, p.x - center.x) - std::atan2(t.DragStart.y - center.y, t.DragStart.x - center.x);
            if (io.KeyShift) angle = std::round(angle / glm::radians(15.0f)) * glm::radians(15.0f);
            for (int k = 0; k < 4; ++k) Q[k] = Rotate(S[k], center, angle);
        }
        else if (t.Drag < 4)
        {
            const int k = t.Drag;
            if (io.KeyCtrl) Q[k] = S[k] + delta; // (distort: the corner alone)
            else
            {
                glm::vec2 o = io.KeyAlt ? center : S[(k + 2) % 4];
                glm::vec2 a0 = Solve(u, v, S[k] - o), a1 = Solve(u, v, S[k] + delta - o);
                float su = std::abs(a0.x) > 1e-6f ? a1.x / a0.x : 1.0f, sv = std::abs(a0.y) > 1e-6f ? a1.y / a0.y : 1.0f;
                if (!io.KeyShift) su = sv = (su + sv) * 0.5f; // (in proportion, as Photoshop does; Shift frees it)
                scaleAbout(o, su, sv);
            }
        }
        else
        {
            const int e = t.Drag - 4, a = e, b = (e + 1) % 4;
            if (io.KeyCtrl)
            {
                // Skew: the side slides along itself.
                glm::vec2 dir = glm::normalize(S[b] - S[a]);
                glm::vec2 along = dir * glm::dot(delta, dir);
                Q[a] = S[a] + along;
                Q[b] = S[b] + along;
            }
            else
            {
                glm::vec2 handle = (S[a] + S[b]) * 0.5f;
                glm::vec2 o = io.KeyAlt ? center : (S[(a + 2) % 4] + S[(b + 2) % 4]) * 0.5f;
                glm::vec2 a0 = Solve(u, v, handle - o), a1 = Solve(u, v, handle + delta - o);
                bool horizontal = e == 1 || e == 3; // (the right and left sides stretch along u)
                float s = horizontal ? (std::abs(a0.x) > 1e-6f ? a1.x / a0.x : 1.0f) : (std::abs(a0.y) > 1e-6f ? a1.y / a0.y : 1.0f);
                scaleAbout(o, horizontal ? s : 1.0f, horizontal ? 1.0f : s);
            }
        }
        bool changed = false;
        for (int k = 0; k < 4; ++k) changed |= Q[k] != t.Quad[k];
        if (changed)
        {
            std::copy(Q, Q + 4, t.Quad);
            t.Dirty = true;
        }
    }

    // ---- Crop

    void ImageMode::CropInput(const glm::vec2& p, bool hovered)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!m_CropActive)
        {
            m_CropActive = true;
            m_CropA = glm::vec2(0.0f);
            m_CropB = glm::vec2((float)m_Doc.Width, (float)m_Doc.Height);
        }
        auto screen = [&](const glm::vec2& q) { return glm::vec2(m_CanvasMin.x + q.x * m_Zoom, m_CanvasMin.y + q.y * m_Zoom); };
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        glm::vec2 lo = glm::min(m_CropA, m_CropB), hi = glm::max(m_CropA, m_CropB);
        const glm::vec2 corners[4] = { lo, { hi.x, lo.y }, hi, { lo.x, hi.y } };
        if (hovered && m_CropDrag < 0)
        {
            int h = -1;
            for (int k = 0; k < 4 && h < 0; ++k)
                if (glm::length(screen(corners[k]) - mouse) < 9.0f) h = k;
            for (int k = 0; k < 4 && h < 0; ++k)
                if (glm::length(screen((corners[k] + corners[(k + 1) % 4]) * 0.5f) - mouse) < 9.0f) h = 4 + k;
            if (h < 0) h = p.x > lo.x && p.y > lo.y && p.x < hi.x && p.y < hi.y ? 8 : 9;
            ImGui::SetMouseCursor(h == 8 ? ImGuiMouseCursor_ResizeAll : h < 8 ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_Arrow);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && h == 8)
            {
                CommitCrop();
                return;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_CropDrag = h;
                m_CropDragStart = p;
                m_CropStartA = lo;
                m_CropStartB = hi;
            }
        }
        if (m_CropDrag < 0) return;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_CropDrag = -1;
            return;
        }
        const glm::vec2 d = glm::round(p - m_CropDragStart);
        glm::vec2 a = m_CropStartA, b = m_CropStartB;
        switch (m_CropDrag)
        {
        case 8: a += d, b += d; break;
        case 9: a = glm::round(m_CropDragStart), b = glm::round(p); break;
        case 0: a += d; break;
        case 1: b.x += d.x, a.y += d.y; break;
        case 2: b += d; break;
        case 3: a.x += d.x, b.y += d.y; break;
        case 4: a.y += d.y; break;
        case 5: b.x += d.x; break;
        case 6: b.y += d.y; break;
        case 7: a.x += d.x; break;
        default: break;
        }
        m_CropA = a;
        m_CropB = b;
    }

    void ImageMode::CommitCrop()
    {
        glm::vec2 lo = glm::round(glm::min(m_CropA, m_CropB)), hi = glm::round(glm::max(m_CropA, m_CropB));
        int w = (int)(hi.x - lo.x), h = (int)(hi.y - lo.y);
        if (w < 1 || h < 1) return;
        if (lo == glm::vec2(0.0f) && w == (int)m_Doc.Width && h == (int)m_Doc.Height) return;
        CanvasChange("Crop", [&] { Reframe(m_Doc, (uint32_t)w, (uint32_t)h, glm::ivec2(-(int)lo.x, -(int)lo.y)); });
        m_CropA = glm::vec2(0.0f);
        m_CropB = glm::vec2((float)m_Doc.Width, (float)m_Doc.Height);
    }

    void ImageMode::CanvasChange(const char* label, const std::function<void()>& change)
    {
        CommitTransform();
        EndPaint();
        Record(label);
        change();
        m_HasCloneSource = m_CloneOffsetSet = m_HasLastPaint = false;
        m_LastSelection.reset();
        m_FitPending = true;
        Changed();
        MarkAll();
    }

    // ---- The clipboard

    void ImageMode::Copy(bool merged)
    {
        if (m_Doc.Layers.empty()) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (!merged && (!l.IsRaster() || !l.Pixels))
        {
            Note("This layer has no pixels of its own: Copy Merged (Ctrl+Shift+C) copies what shows");
            return;
        }
        Rect bounds;
        m_Clipboard = CopyPixels(merged ? m_Composite : l.Get(), m_Doc.Selection.get(), bounds);
        if (m_Clipboard.Empty())
        {
            Note("Nothing to copy there");
            return;
        }
        m_ClipboardAt = { bounds.X0, bounds.Y0 };
        m_ClipboardCanvas = { m_Doc.Width, m_Doc.Height };
        Note("Copied " + std::to_string(m_Clipboard.Width) + " x " + std::to_string(m_Clipboard.Height) + " px");
    }

    void ImageMode::Cut()
    {
        if (!m_Doc.Selection)
        {
            Note("Select what to cut first");
            return;
        }
        Copy(false);
        if (!m_Clipboard.Empty()) Clear();
    }

    void ImageMode::Paste(bool inPlace)
    {
        if (m_Clipboard.Empty())
        {
            Note("Nothing to paste: copy something first");
            return;
        }
        CommitTransform();
        // Where it was (in place, or when it came from a canvas the same size), else in the middle.
        glm::ivec2 at = m_ClipboardAt;
        if (!inPlace && m_ClipboardCanvas != glm::uvec2(m_Doc.Width, m_Doc.Height))
            at = glm::ivec2(((int)m_Doc.Width - (int)m_Clipboard.Width) / 2, ((int)m_Doc.Height - (int)m_Clipboard.Height) / 2);
        Record("Paste");
        int index = m_ActiveLayer + 1;
        uint32_t parent = m_Doc.Layers.empty() ? 0 : m_Doc.Layers[(size_t)m_ActiveLayer].Parent;
        index = m_Doc.AddLayer(m_Doc.UniqueLayerName("Layer 1"), index, parent);
        Blit(m_Doc.Layers[(size_t)index].Edit(), m_Clipboard, at);
        if (m_Doc.Selection) m_LastSelection = m_Doc.Selection;
        m_Doc.Selection.reset();
        SetActive(index);
        Changed();
        MarkAll();
    }

    void ImageMode::Clear()
    {
        if (!m_Doc.Selection)
        {
            Note("Select what to clear first");
            return;
        }
        if (m_EditMask)
        {
            Record("Clear (Mask)");
            Mark(FillMask(m_Doc.Layers[(size_t)m_ActiveLayer].EditMask(), m_Doc.Selection.get(), 0));
            Changed();
            return;
        }
        if (!CanEditTarget(Tool::Eraser)) return;
        Record("Clear");
        Rect r = ClearPixels(m_Doc.Layers[(size_t)m_ActiveLayer].Edit(), m_Doc.Selection.get());
        if (r.Empty()) m_Undo.pop_back();
        else Mark(r), Changed();
    }

    void ImageMode::FillWith(const glm::vec4& color, const char* label)
    {
        if (!CanEditTarget(Tool::Brush)) return;
        Record(label);
        Rect r = FillSelection(m_Doc, m_ActiveLayer, color, m_Doc.Selection.get(), m_EditMask);
        if (r.Empty()) m_Undo.pop_back();
        else Mark(r), Changed();
    }

    void ImageMode::LayerVia(bool cut)
    {
        if (!m_Doc.Selection) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (!l.HasPixels() || (cut && (l.LockAll || l.LockPixels)))
        {
            Note(!l.HasPixels() ? "Select a pixel layer" : "The layer is locked");
            return;
        }
        Rect bounds;
        Bitmap piece = CopyPixels(l.Get(), m_Doc.Selection.get(), bounds);
        if (piece.Empty())
        {
            Note("Nothing selected on this layer");
            return;
        }
        Record(cut ? "Layer via Cut" : "Layer via Copy");
        if (cut) ClearPixels(m_Doc.Layers[(size_t)m_ActiveLayer].Edit(), m_Doc.Selection.get());
        int index = m_Doc.AddLayer(m_Doc.UniqueLayerName("Layer 1"), m_ActiveLayer + 1, l.Parent);
        Blit(m_Doc.Layers[(size_t)index].Edit(), piece, { bounds.X0, bounds.Y0 });
        m_LastSelection = m_Doc.Selection;
        m_Doc.Selection.reset();
        SetActive(index);
        Changed();
        MarkAll();
    }

    void ImageMode::TransformLayer(int kind)
    {
        if (!BeginTransform()) return;
        TransformState& t = m_Transform;
        glm::vec2* q = t.Quad;
        const glm::vec2 c = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
        glm::vec2 r[4];
        switch (kind)
        {
        case 0: r[0] = q[1], r[1] = q[0], r[2] = q[3], r[3] = q[2]; break;
        case 1: r[0] = q[3], r[1] = q[2], r[2] = q[1], r[3] = q[0]; break;
        default:
        {
            float angle = kind == 2 ? glm::half_pi<float>() : kind == 3 ? -glm::half_pi<float>() : glm::pi<float>();
            for (int k = 0; k < 4; ++k) r[k] = glm::round(Rotate(q[k], glm::round(c), angle) * 2.0f) * 0.5f;
        }
        }
        std::copy(r, r + 4, q);
        t.Method = Resample::Nearest; // (exact pixels for flips and quarter turns)
        t.Dirty = true;
        CommitTransform();
    }

    // ---- Overlays: the selection's marching ants, shapes being drawn, the transform's box and the crop

    void ImageMode::DrawOverlay(ImDrawList* dl, const glm::vec2& mouse)
    {
        auto screen = [&](const glm::vec2& q) { return ImVec2(m_CanvasMin.x + q.x * m_Zoom, m_CanvasMin.y + q.y * m_Zoom); };
        const float phase = (float)std::fmod(ImGui::GetTime() * 16.0, 8.0);
        // The selection.
        if (m_Doc.Selection && !m_Move.Active)
        {
            if (m_OutlineOf != m_Doc.Selection)
            {
                m_Outline = Outline(*m_Doc.Selection, 200000);
                m_OutlineOf = m_Doc.Selection;
            }
            const ImVec2 clipMin = dl->GetClipRectMin(), clipMax = dl->GetClipRectMax();
            for (const Segment& s : m_Outline)
            {
                ImVec2 a = screen(glm::vec2(s.A)), b = screen(glm::vec2(s.B));
                if (std::max(a.x, b.x) < clipMin.x || std::min(a.x, b.x) > clipMax.x || std::max(a.y, b.y) < clipMin.y || std::min(a.y, b.y) > clipMax.y) continue;
                Dashed(dl, ImVec2(std::floor(a.x) + 0.5f, std::floor(a.y) + 0.5f), ImVec2(std::floor(b.x) + 0.5f, std::floor(b.y) + 0.5f), phase);
            }
        }
        else if (!m_Doc.Selection)
            m_OutlineOf.reset();
        // A marquee being dragged.
        if (m_SelDrag)
        {
            const ImGuiIO& io = ImGui::GetIO();
            glm::vec2 a = m_SelStart, b = m_SelEnd;
            if (io.KeyShift && !(m_DragMods & 1))
            {
                float side = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                b = a + glm::vec2(b.x >= a.x ? side : -side, b.y >= a.y ? side : -side);
            }
            if (io.KeyAlt && !(m_DragMods & 2)) a = a - (b - a);
            ImVec2 sa = screen(glm::min(a, b)), sb = screen(glm::max(a, b));
            if (m_Tool == Tool::EllipseMarquee)
            {
                ImVec2 c((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
                dl->AddEllipse(c, ImVec2((sb.x - sa.x) * 0.5f, (sb.y - sa.y) * 0.5f), IM_COL32(0, 0, 0, 255), 0.0f, 0, 2.0f);
                dl->AddEllipse(c, ImVec2((sb.x - sa.x) * 0.5f, (sb.y - sa.y) * 0.5f), IM_COL32(255, 255, 255, 255));
            }
            else
            {
                Dashed(dl, sa, ImVec2(sb.x, sa.y), phase);
                Dashed(dl, ImVec2(sb.x, sa.y), sb, phase);
                Dashed(dl, ImVec2(sa.x, sb.y), sb, phase);
                Dashed(dl, sa, ImVec2(sa.x, sb.y), phase);
            }
        }
        // A lasso being drawn.
        if (m_LassoActive && !m_Lasso.empty())
        {
            std::vector<ImVec2> pts;
            for (const glm::vec2& p : m_Lasso) pts.push_back(screen(p));
            if (m_Tool == Tool::PolyLasso) pts.push_back(screen(mouse));
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(0, 0, 0, 255), 0, 2.0f);
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 255), 0, 1.0f);
            if (m_Tool == Tool::PolyLasso) dl->AddCircle(pts.front(), 5.0f, IM_COL32(255, 255, 255, 255));
        }
        // The transform's box and handles.
        if (m_Transform.Active)
        {
            ImVec2 q[4];
            for (int k = 0; k < 4; ++k) q[k] = screen(m_Transform.Quad[k]);
            dl->AddPolyline(q, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 3.0f);
            dl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 1.0f);
            auto handle = [&](ImVec2 c) {
                dl->AddRectFilled(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), IM_COL32(255, 255, 255, 255));
                dl->AddRect(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), IM_COL32(0, 0, 0, 255));
            };
            for (int k = 0; k < 4; ++k)
            {
                handle(q[k]);
                handle(ImVec2((q[k].x + q[(k + 1) % 4].x) * 0.5f, (q[k].y + q[(k + 1) % 4].y) * 0.5f));
            }
            ImVec2 c((q[0].x + q[1].x + q[2].x + q[3].x) * 0.25f, (q[0].y + q[1].y + q[2].y + q[3].y) * 0.25f);
            dl->AddCircle(c, 4.0f, IM_COL32(255, 255, 255, 255), 0, 1.5f);
        }
        // The crop: the parts cut away darkened, the thirds, the handles.
        if (m_Tool == Tool::Crop && m_CropActive)
        {
            glm::vec2 lo = glm::min(m_CropA, m_CropB), hi = glm::max(m_CropA, m_CropB);
            ImVec2 a = screen(lo), b = screen(hi);
            const ImVec2 cmin = dl->GetClipRectMin(), cmax = dl->GetClipRectMax();
            const ImU32 shade = IM_COL32(0, 0, 0, 140);
            dl->AddRectFilled(cmin, ImVec2(cmax.x, a.y), shade);
            dl->AddRectFilled(ImVec2(cmin.x, b.y), cmax, shade);
            dl->AddRectFilled(ImVec2(cmin.x, a.y), ImVec2(a.x, b.y), shade);
            dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(cmax.x, b.y), shade);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.5f);
            for (int k = 1; k < 3; ++k)
            {
                float x = a.x + (b.x - a.x) * k / 3.0f, y = a.y + (b.y - a.y) * k / 3.0f;
                dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 90));
                dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 90));
            }
            ImVec2 hs[8] = { a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), ImVec2((a.x + b.x) * 0.5f, a.y), ImVec2(b.x, (a.y + b.y) * 0.5f),
                             ImVec2((a.x + b.x) * 0.5f, b.y), ImVec2(a.x, (a.y + b.y) * 0.5f) };
            for (ImVec2 h : hs) dl->AddRectFilled(ImVec2(h.x - 4, h.y - 4), ImVec2(h.x + 4, h.y + 4), IM_COL32(255, 255, 255, 255));
        }
    }

    // ---- Options bars

    void ImageMode::DrawSelectOptions()
    {
        static const char* ops[] = { "New", "Add", "Subtract", "Intersect" };
        static const char* tips[] = { "A new selection", "Add to the selection (Shift)", "Subtract from the selection (Alt)",
                                      "Intersect with the selection (Shift+Alt)" };
        for (int i = 0; i < 4; ++i)
        {
            bool on = (int)m_SelectOp == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(ops[i])) m_SelectOp = (SelectOp)i;
            if (on) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tips[i]);
            ImGui::SameLine(0.0f, 2.0f);
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (m_Tool == Tool::MagicWand)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Tolerance");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("##wandTolerance", &m_WandTolerance, 0.0f, 255.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::Checkbox("Contiguous", &m_WandContiguous);
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::Checkbox("Sample All Layers", &m_WandSampleAll);
            return;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Feather");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##feather", &m_SelFeather, 0.0f, 250.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::Checkbox("Anti-alias", &m_SelAntialias);
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled(m_Tool == Tool::PolyLasso ? "Click the corners; click the first one (or double-click, Enter) to close; Backspace takes one back"
                                                      : "Drag inside the selection to move it; arrows nudge it");
    }

    void ImageMode::DrawTransformOptions()
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(UI::AccentText(), "Free Transform");
        ImGui::SameLine(0.0f, 16.0f);
        auto quarter = [&](int kind) {
            glm::vec2* q = m_Transform.Quad;
            const glm::vec2 c = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
            glm::vec2 r[4];
            if (kind == 0) r[0] = q[1], r[1] = q[0], r[2] = q[3], r[3] = q[2];
            else if (kind == 1) r[0] = q[3], r[1] = q[2], r[2] = q[1], r[3] = q[0];
            else
                for (int k = 0; k < 4; ++k) r[k] = Rotate(q[k], c, kind == 2 ? glm::half_pi<float>() : -glm::half_pi<float>());
            std::copy(r, r + 4, q);
            m_Transform.Dirty = true;
        };
        if (ImGui::Button("Flip H")) quarter(0);
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Flip V")) quarter(1);
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Rotate 90 CW")) quarter(2);
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Rotate 90 CCW")) quarter(3);
        ImGui::SameLine(0.0f, 12.0f);
        int method = m_Transform.Method == Resample::Nearest ? 0 : 1;
        static const char* methods[] = { "Nearest (hard pixels)", "Bilinear (smooth)" };
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("##interp", &method, methods, 2))
        {
            m_Transform.Method = method == 0 ? Resample::Nearest : Resample::Bilinear;
            m_Transform.Dirty = true;
        }
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button("Apply")) CommitTransform();
        ImGui::SetItemTooltip("Enter (or double-click inside)");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Cancel")) CancelTransform();
        ImGui::SetItemTooltip("Esc");
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("Corners scale (Shift: freely), sides stretch, outside rotates (Shift: 15 deg); Ctrl: distort / skew; Alt: from the center");
    }

    void ImageMode::DrawCropOptions()
    {
        glm::vec2 lo = glm::round(glm::min(m_CropA, m_CropB)), hi = glm::round(glm::max(m_CropA, m_CropB));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d x %d px", (int)(hi.x - lo.x), (int)(hi.y - lo.y));
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button("Apply")) CommitCrop();
        ImGui::SetItemTooltip("Enter (or double-click inside)");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Reset"))
        {
            m_CropA = glm::vec2(0.0f);
            m_CropB = glm::vec2((float)m_Doc.Width, (float)m_Doc.Height);
        }
        ImGui::SetItemTooltip("Esc");
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("Drag the edges and corners (past the canvas makes it larger), or drag a new box outside");
    }

    // ---- Menus and dialogs

    void ImageMode::DrawMenuBar()
    {
        if (!ImGui::BeginMenuBar()) return;
        const bool sel = m_Doc.Selection != nullptr;
        const bool group = !m_Doc.Layers.empty() && !m_Doc.Layers[(size_t)m_ActiveLayer].HasPixels(); // (no pixels: a group or an adjustment)
        const bool isGroup = !m_Doc.Layers.empty() && m_Doc.Layers[(size_t)m_ActiveLayer].IsGroup();
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_Undo.empty())) Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !m_Redo.empty())) Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X", false, sel)) Cut();
            if (ImGui::MenuItem("Copy", "Ctrl+C")) Copy(false);
            if (ImGui::MenuItem("Copy Merged", "Ctrl+Shift+C")) Copy(true);
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_Clipboard.Empty())) Paste(false);
            if (ImGui::MenuItem("Paste in Place", "Ctrl+Shift+V", false, !m_Clipboard.Empty())) Paste(true);
            if (ImGui::MenuItem("Clear", "Delete", false, sel)) Clear();
            ImGui::Separator();
            if (ImGui::MenuItem("Fill with Foreground", "Alt+Backspace")) FillWith(m_Foreground, "Fill");
            if (ImGui::MenuItem("Fill with Background", "Ctrl+Backspace")) FillWith(m_Background, "Fill");
            ImGui::Separator();
            if (ImGui::MenuItem("Free Transform", "Ctrl+T")) BeginTransform();
            if (ImGui::BeginMenu("Transform"))
            {
                if (ImGui::MenuItem("Rotate 180")) TransformLayer(4);
                if (ImGui::MenuItem("Rotate 90 Clockwise")) TransformLayer(2);
                if (ImGui::MenuItem("Rotate 90 Counterclockwise")) TransformLayer(3);
                if (ImGui::MenuItem("Flip Horizontal")) TransformLayer(0);
                if (ImGui::MenuItem("Flip Vertical")) TransformLayer(1);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Image"))
        {
            if (ImGui::BeginMenu("Adjustments"))
            {
                DrawAdjustmentMenu(false);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reduce Colors..."))
            {
                OpenFilter(FilterType::ReduceColors);
            }
            ImGui::SetItemTooltip("To a palette (the Swatches' or the picture's own colors), dithered or not: pixel art");
            ImGui::Separator();
            if (ImGui::MenuItem("Image Size...", "Ctrl+Alt+I")) m_Dialog = 1, m_OpenDialog = true;
            if (ImGui::MenuItem("Canvas Size...", "Ctrl+Alt+C")) m_Dialog = 2, m_OpenDialog = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Rotate 90 Clockwise")) CanvasChange("Rotate Canvas", [&] { RotateCanvas(m_Doc, 1); });
            if (ImGui::MenuItem("Rotate 90 Counterclockwise")) CanvasChange("Rotate Canvas", [&] { RotateCanvas(m_Doc, -1); });
            if (ImGui::MenuItem("Rotate 180")) CanvasChange("Rotate Canvas", [&] { RotateCanvas(m_Doc, 2); });
            if (ImGui::MenuItem("Flip Canvas Horizontal")) CanvasChange("Flip Canvas", [&] { FlipCanvas(m_Doc, true); });
            if (ImGui::MenuItem("Flip Canvas Vertical")) CanvasChange("Flip Canvas", [&] { FlipCanvas(m_Doc, false); });
            ImGui::Separator();
            if (ImGui::MenuItem("Crop to Selection", nullptr, false, sel))
            {
                Rect b = Bounds(*m_Doc.Selection);
                if (!b.Empty())
                    CanvasChange("Crop", [&] { Reframe(m_Doc, (uint32_t)(b.X1 - b.X0 + 1), (uint32_t)(b.Y1 - b.Y0 + 1), glm::ivec2(-b.X0, -b.Y0)); });
            }
            if (ImGui::MenuItem("Sprite Sheet...")) m_Dialog = 8, m_OpenDialog = true;
            ImGui::SetItemTooltip("Slice the picture into sprites (a grid, or its separate shapes) for the texture");
            if (ImGui::MenuItem("Export As...")) m_Dialog = 9, m_OpenDialog = true;
            ImGui::SetItemTooltip("Another file: a format, a scale (2x nearest for pixel art)");
            ImGui::Separator();
            if (ImGui::MenuItem("Trim Transparent Pixels"))
            {
                Rect b = ContentBounds(m_Doc.Flatten());
                if (!b.Empty())
                    CanvasChange("Trim", [&] { Reframe(m_Doc, (uint32_t)(b.X1 - b.X0 + 1), (uint32_t)(b.Y1 - b.Y0 + 1), glm::ivec2(-b.X0, -b.Y0)); });
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layer"))
        {
            if (ImGui::MenuItem("New Layer", "Ctrl+Shift+N")) NewLayer();
            if (ImGui::MenuItem(isGroup ? "Duplicate Group" : "Duplicate Layer")) DuplicateLayer();
            if (ImGui::MenuItem("Layer via Copy", "Ctrl+J", false, sel && !group)) LayerVia(false);
            if (ImGui::MenuItem("Layer via Cut", "Ctrl+Shift+J", false, sel && !group)) LayerVia(true);
            if (ImGui::MenuItem(isGroup ? "Delete Group" : "Delete Layer")) DeleteLayer();
            ImGui::Separator();
            if (ImGui::MenuItem("Group", "Ctrl+G")) GroupLayer();
            if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, isGroup)) Ungroup();
            if (ImGui::MenuItem(isGroup ? "Merge Group" : "Merge Down", "Ctrl+E")) MergeDown();
            ImGui::Separator();
            if (ImGui::BeginMenu("New Adjustment Layer"))
            {
                DrawAdjustmentMenu(true);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Layer Style...", nullptr, false, !m_Doc.Layers.empty() && m_Doc.Layers[(size_t)m_ActiveLayer].IsRaster())) OpenLayerStyle();
            if (ImGui::MenuItem("Rasterize", "Ctrl+Shift+R", false, !m_Doc.Layers.empty() && m_Doc.Layers[(size_t)m_ActiveLayer].IsVector())) RasterizeLayer();
            if (ImGui::MenuItem("Clear Layer Style", nullptr, false, !m_Doc.Layers.empty() && m_Doc.Layers[(size_t)m_ActiveLayer].Effects.Any()))
            {
                Record("Clear Layer Style");
                m_Doc.Layers[(size_t)m_ActiveLayer].Effects = LayerEffects();
                Changed();
                MarkAll();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(sel ? "Add Mask: Reveal Selection" : "Add Mask: Reveal All")) AddMask(true);
            if (ImGui::MenuItem(sel ? "Add Mask: Hide Selection" : "Add Mask: Hide All")) AddMask(false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Select"))
        {
            if (ImGui::MenuItem("All", "Ctrl+A")) SelectAll();
            if (ImGui::MenuItem("Deselect", "Ctrl+D", false, sel)) Deselect();
            if (ImGui::MenuItem("Reselect", "Ctrl+Shift+D", false, m_LastSelection != nullptr)) Reselect();
            if (ImGui::MenuItem("Inverse", "Ctrl+Shift+I", false, sel)) InverseSelection();
            ImGui::Separator();
            if (ImGui::MenuItem("Color Range...")) m_Dialog = 3, m_OpenDialog = true, m_DialogValue = 40.0f;
            if (ImGui::MenuItem("Layer Transparency", nullptr, false, !group))
                SetSelection(SelectAlpha(m_Doc.Layers[(size_t)m_ActiveLayer].Get()), SelectOp::Replace, "Load Selection");
            ImGui::SetItemTooltip("The layer's pixels as a selection (also Ctrl+click its thumbnail)");
            ImGui::Separator();
            if (ImGui::BeginMenu("Modify", sel))
            {
                if (ImGui::MenuItem("Border...")) m_Dialog = 7, m_OpenDialog = true, m_DialogValue = 4.0f;
                if (ImGui::MenuItem("Expand...")) m_Dialog = 4, m_OpenDialog = true, m_DialogValue = 4.0f;
                if (ImGui::MenuItem("Contract...")) m_Dialog = 5, m_OpenDialog = true, m_DialogValue = 4.0f;
                if (ImGui::MenuItem("Feather...", "Shift+F6")) m_Dialog = 6, m_OpenDialog = true, m_DialogValue = 4.0f;
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Filter"))
        {
            DrawFilterMenu();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void ImageMode::DrawDialogs()
    {
        static const char* names[] = { "", "Image Size", "Canvas Size", "Color Range", "Expand Selection", "Contract Selection", "Feather Selection",
                                       "Border Selection" };
        if (m_Dialog == 8 || m_Dialog == 9)
        {
            DrawGameDialog();
            return;
        }
        if (m_Dialog <= 0 || m_Dialog > 7) return;
        const char* name = names[m_Dialog];
        if (m_OpenDialog)
        {
            m_OpenDialog = false;
            m_DialogSize[0] = (int)m_Doc.Width;
            m_DialogSize[1] = (int)m_Doc.Height;
            m_DialogAnchor = 4;
            ImGui::OpenPopup(name);
        }
        bool open = true;
        if (!ImGui::BeginPopupModal(name, &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (!ImGui::IsPopupOpen(name)) m_Dialog = 0;
            return;
        }
        bool apply = false;
        if (m_Dialog == 1 || m_Dialog == 2)
        {
            int before[2] = { m_DialogSize[0], m_DialogSize[1] };
            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputInt2("Size (px)", m_DialogSize);
            m_DialogSize[0] = std::clamp(m_DialogSize[0], 1, 16384);
            m_DialogSize[1] = std::clamp(m_DialogSize[1], 1, 16384);
            if (m_Dialog == 1)
            {
                ImGui::Checkbox("Constrain proportions", &m_DialogConstrain);
                if (m_DialogConstrain)
                {
                    if (m_DialogSize[0] != before[0]) m_DialogSize[1] = std::max(1, (int)std::lround(m_DialogSize[0] * (double)m_Doc.Height / m_Doc.Width));
                    else if (m_DialogSize[1] != before[1]) m_DialogSize[0] = std::max(1, (int)std::lround(m_DialogSize[1] * (double)m_Doc.Width / m_Doc.Height));
                }
                for (int pct : { 25, 50, 200, 400 })
                {
                    if (ImGui::SmallButton((std::to_string(pct) + "%").c_str()))
                        m_DialogSize[0] = std::max(1, (int)m_Doc.Width * pct / 100), m_DialogSize[1] = std::max(1, (int)m_Doc.Height * pct / 100);
                    ImGui::SameLine();
                }
                ImGui::NewLine();
                static const char* methods[] = { "Nearest Neighbor (hard edges: pixel art)", "Bilinear (smooth)" };
                ImGui::SetNextItemWidth(280.0f);
                ImGui::Combo("Resample", &m_DialogResample, methods, 2);
            }
            else
            {
                ImGui::TextUnformatted("Anchor (where the image stays)");
                for (int k = 0; k < 9; ++k)
                {
                    ImGui::PushID(k);
                    bool on = m_DialogAnchor == k;
                    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button(on ? "o" : " ", ImVec2(28, 28))) m_DialogAnchor = k;
                    if (on) ImGui::PopStyleColor();
                    ImGui::PopID();
                    if (k % 3 != 2) ImGui::SameLine(0.0f, 2.0f);
                }
                ImGui::TextDisabled("New areas are transparent.");
            }
        }
        else if (m_Dialog == 3)
        {
            ImGui::TextUnformatted("Selects the pixels close to the foreground color:");
            ImGui::SameLine();
            ImGui::ColorButton("##fg", ImVec4(m_Foreground.r, m_Foreground.g, m_Foreground.b, 1.0f), 0, ImVec2(20, 20));
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("Fuzziness", &m_DialogValue, 0.0f, 200.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextDisabled("(pick the color with the eyedropper first)");
        }
        else
        {
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat(m_Dialog == 6 ? "Feather Radius" : m_Dialog == 7 ? "Width" : "By", &m_DialogValue, 1.0f, 100.0f, "%.0f px",
                               ImGuiSliderFlags_AlwaysClamp);
        }
        if (ImGui::Button("OK", ImVec2(100, 0))) apply = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)) || !open)
        {
            ImGui::CloseCurrentPopup();
            m_Dialog = 0;
        }
        if (apply)
        {
            const int kind = m_Dialog;
            const uint32_t w = (uint32_t)m_DialogSize[0], h = (uint32_t)m_DialogSize[1];
            if (kind == 1 && (w != m_Doc.Width || h != m_Doc.Height))
                CanvasChange("Image Size", [&] { ResizeImage(m_Doc, w, h, m_DialogResample == 0 ? Resample::Nearest : Resample::Bilinear); });
            else if (kind == 2 && (w != m_Doc.Width || h != m_Doc.Height))
            {
                int ax = m_DialogAnchor % 3, ay = m_DialogAnchor / 3;
                glm::ivec2 offset(ax == 0 ? 0 : ax == 1 ? ((int)w - (int)m_Doc.Width) / 2 : (int)w - (int)m_Doc.Width,
                                  ay == 0 ? 0 : ay == 1 ? ((int)h - (int)m_Doc.Height) / 2 : (int)h - (int)m_Doc.Height);
                CanvasChange("Canvas Size", [&] { Reframe(m_Doc, w, h, offset); });
            }
            else if (kind == 3)
                SetSelection(SelectColorRange(m_Composite, m_Foreground, m_DialogValue), SelectOp::Replace, "Color Range");
            else if (kind >= 4)
                ModifySelection(kind, m_DialogValue);
            ImGui::CloseCurrentPopup();
            m_Dialog = 0;
        }
        ImGui::EndPopup();
    }
}
