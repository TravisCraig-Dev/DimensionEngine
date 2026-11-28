// Image Mode: the Type tool (text typed on the canvas) and the shape tools, their options, and the Properties of text
// and shape layers.
#include "ImageMode.h"
#include "EditorUI.h"
#include <Dimension/Asset/AssetManager.h>
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
        std::string Utf8(uint32_t cp)
        {
            std::string s;
            if (cp < 0x80) s += (char)cp;
            else if (cp < 0x800) s += (char)(0xC0 | (cp >> 6)), s += (char)(0x80 | (cp & 0x3F));
            else if (cp < 0x10000) s += (char)(0xE0 | (cp >> 12)), s += (char)(0x80 | ((cp >> 6) & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
            else s += (char)(0xF0 | (cp >> 18)), s += (char)(0x80 | ((cp >> 12) & 0x3F)), s += (char)(0x80 | ((cp >> 6) & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
            return s;
        }
        size_t PrevChar(const std::string& s, size_t i)
        {
            if (i == 0) return 0;
            --i;
            while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
            return i;
        }
        size_t NextChar(const std::string& s, size_t i)
        {
            if (i >= s.size()) return s.size();
            ++i;
            while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
            return i;
        }
        bool InQuad(const std::array<glm::vec2, 4>& q, const glm::vec2& p)
        {
            bool pos = true, neg = true;
            for (int k = 0; k < 4; ++k)
            {
                glm::vec2 e = q[(size_t)(k + 1) % 4] - q[(size_t)k], d = p - q[(size_t)k];
                float c = e.x * d.y - e.y * d.x;
                pos &= c >= 0.0f;
                neg &= c <= 0.0f;
            }
            return pos || neg;
        }
        std::string NameFor(const std::string& text)
        {
            std::string name = text.substr(0, std::min(text.find('\n'), (size_t)30));
            return name.empty() ? "Text" : name;
        }
        // The caret (byte) nearest to a point in the text's own frame.
        size_t CaretAt(const TextLayout& layout, const glm::vec2& local)
        {
            if (layout.Lines.empty()) return 0;
            size_t best = 0;
            float bestLine = 1e30f;
            for (size_t i = 0; i < layout.Lines.size(); ++i)
            {
                float mid = layout.Lines[i].Baseline - layout.Ascent * 0.35f;
                if (std::abs(local.y - mid) < bestLine) bestLine = std::abs(local.y - mid), best = i;
            }
            const auto& line = layout.Lines[best];
            size_t k = 0;
            float bestX = 1e30f;
            for (size_t i = 0; i < line.Carets.size(); ++i)
                if (float d = std::abs(local.x - (line.X + line.Carets[i])); d < bestX) bestX = d, k = i;
            return line.Offsets[k];
        }
        // Where the caret is: its line and its x.
        bool CaretPlace(const TextLayout& layout, size_t caret, size_t& lineIndex, float& x)
        {
            for (size_t i = 0; i < layout.Lines.size(); ++i)
            {
                const auto& line = layout.Lines[i];
                for (size_t k = 0; k < line.Offsets.size(); ++k)
                    if (line.Offsets[k] == caret)
                    {
                        lineIndex = i;
                        x = line.X + line.Carets[k];
                        return true;
                    }
            }
            return false;
        }
    }

    bool ImageMode::IsShapeTool(Tool tool)
    {
        return tool == Tool::RectShape || tool == Tool::RoundShape || tool == Tool::EllipseShape || tool == Tool::PolygonShape ||
               tool == Tool::StarShape || tool == Tool::LineShape;
    }

    void ImageMode::EditVector(const std::function<bool(Layer&)>& change, const char* label)
    {
        if (m_Doc.Layers.empty()) return;
        if (!m_VectorRecorded)
        {
            Record(label);
            m_VectorRecorded = true;
        }
        if (change(m_Doc.Layers[(size_t)m_ActiveLayer]))
        {
            Changed();
            MarkAll();
        }
    }

    void ImageMode::RasterizeLayer()
    {
        if (m_Doc.Layers.empty() || !m_Doc.Layers[(size_t)m_ActiveLayer].IsVector()) return;
        CommitText();
        Record("Rasterize");
        m_Doc.Rasterize(m_ActiveLayer);
        Changed();
        MarkAll();
    }

    // ---- Text

    void ImageMode::BeginTextEdit(int index, const glm::vec2& p)
    {
        Layer& l = m_Doc.Layers[(size_t)index];
        Record("Edit Text");
        SetActive(index);
        m_TextEditing = true;
        m_TextNew = false;
        m_TextLayer = l.Id;
        m_TextBefore = l.Text.Text;
        m_Caret = CaretAt(LayoutText(l.Text), CanvasToText(l.Text, p));
    }

    void ImageMode::CommitText()
    {
        if (!m_TextEditing) return;
        m_TextEditing = false;
        int i = m_Doc.IndexOf(m_TextLayer);
        if (i < 0) return;
        Layer& l = m_Doc.Layers[(size_t)i];
        if (l.Text.Text.empty())
        {
            // Nothing typed: no layer (a new one goes with its undo step; an old one emptied is deleted).
            m_Doc.RemoveLayer(i);
            if (m_TextNew && !m_Undo.empty()) m_Undo.pop_back();
            else Changed();
            SetActive(std::clamp(i - 1, 0, std::max(0, (int)m_Doc.Layers.size() - 1)));
            MarkAll();
            return;
        }
        if (!m_TextNew && l.Text.Text == m_TextBefore)
        {
            if (!m_Undo.empty()) m_Undo.pop_back();
            return;
        }
        // The layer's name follows its text until it is renamed.
        if (m_TextNew || l.Name == NameFor(m_TextBefore)) l.Name = NameFor(l.Text.Text);
        Changed();
        MarkAll();
    }

    void ImageMode::TextKeys()
    {
        int i = m_Doc.IndexOf(m_TextLayer);
        if (i < 0)
        {
            m_TextEditing = false;
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        auto pressed = [](ImGuiKeyChord chord) { return ImGui::IsKeyChordPressed(chord, ImGuiInputFlags_Repeat); };
        if (pressed(ImGuiKey_Escape) || pressed(ImGuiMod_Ctrl | ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))
        {
            CommitText();
            return;
        }
        TextData& t = m_Doc.Layers[(size_t)i].Text;
        std::string& s = t.Text;
        m_Caret = std::min(m_Caret, s.size());
        bool changed = false;
        if (!io.KeyCtrl)
            for (ImWchar c : io.InputQueueCharacters)
                if (c >= 32 && c != 127)
                {
                    std::string u = Utf8(c);
                    s.insert(m_Caret, u);
                    m_Caret += u.size();
                    changed = true;
                }
        if (pressed(ImGuiKey_Enter)) s.insert(m_Caret, "\n"), ++m_Caret, changed = true;
        if (pressed(ImGuiKey_Backspace) && m_Caret > 0)
        {
            size_t p = PrevChar(s, m_Caret);
            s.erase(p, m_Caret - p);
            m_Caret = p;
            changed = true;
        }
        if (pressed(ImGuiKey_Delete) && m_Caret < s.size())
        {
            s.erase(m_Caret, NextChar(s, m_Caret) - m_Caret);
            changed = true;
        }
        if (pressed(ImGuiKey_LeftArrow)) m_Caret = PrevChar(s, m_Caret);
        if (pressed(ImGuiKey_RightArrow)) m_Caret = NextChar(s, m_Caret);
        if (pressed(ImGuiKey_Home) || pressed(ImGuiKey_End) || pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow))
        {
            TextLayout layout = LayoutText(t);
            size_t line = 0;
            float x = 0.0f;
            if (CaretPlace(layout, m_Caret, line, x))
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Home)) m_Caret = layout.Lines[line].Start;
                else if (ImGui::IsKeyPressed(ImGuiKey_End)) m_Caret = layout.Lines[line].End;
                else
                {
                    int to = (int)line + (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 1);
                    if (to >= 0 && to < (int)layout.Lines.size()) m_Caret = CaretAt(layout, { x, layout.Lines[(size_t)to].Baseline - layout.Ascent * 0.35f });
                }
            }
        }
        if (changed) MarkAll();
    }

    // ---- The canvas

    void ImageMode::VectorInput(const glm::vec2& p, bool hovered, Tool tool)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (tool == Tool::Type)
        {
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            if (!clicked) return;
            // The topmost text layer under the mouse.
            int hit = -1;
            for (int i = (int)m_Doc.Layers.size() - 1; i >= 0 && hit < 0; --i)
            {
                const Layer& l = m_Doc.Layers[(size_t)i];
                if (l.Kind == LayerKind::Text && m_Doc.ShownInTree(i) && InQuad(TextCorners(l.Text, LayoutText(l.Text)), p)) hit = i;
            }
            if (m_TextEditing)
            {
                int editing = m_Doc.IndexOf(m_TextLayer);
                if (hit >= 0 && hit == editing)
                {
                    m_Caret = CaretAt(LayoutText(m_Doc.Layers[(size_t)hit].Text), CanvasToText(m_Doc.Layers[(size_t)hit].Text, p));
                    return;
                }
                CommitText();
                hit = -1;
                for (int i = (int)m_Doc.Layers.size() - 1; i >= 0 && hit < 0; --i)
                {
                    const Layer& l = m_Doc.Layers[(size_t)i];
                    if (l.Kind == LayerKind::Text && m_Doc.ShownInTree(i) && InQuad(TextCorners(l.Text, LayoutText(l.Text)), p)) hit = i;
                }
            }
            if (hit >= 0)
            {
                BeginTextEdit(hit, p);
                return;
            }
            // A new text layer, starting where clicked.
            Record("Type");
            TextData t = m_TextStyle;
            t.Text.clear();
            t.Position = glm::round(p);
            t.Rotation = 0.0f;
            uint32_t parent = m_Doc.Layers.empty() ? 0 : m_Doc.Layers[(size_t)m_ActiveLayer].Parent;
            int index = m_Doc.AddText(t, m_ActiveLayer + 1, parent);
            SetActive(index);
            m_TextEditing = m_TextNew = true;
            m_TextLayer = m_Doc.Layers[(size_t)index].Id;
            m_TextBefore.clear();
            m_Caret = 0;
            MarkAll();
            return;
        }
        if (!IsShapeTool(tool)) return;
        if (clicked)
        {
            m_ShapeDrag = true;
            m_ShapeStart = m_ShapeEnd = p;
            m_ShapeMods = (io.KeyShift ? 1 : 0) | (io.KeyAlt ? 2 : 0);
        }
        if (!m_ShapeDrag) return;
        m_ShapeEnd = p;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
        m_ShapeDrag = false;
        ShapeData s = m_ShapeStyle;
        glm::vec2 a = m_ShapeStart, b = m_ShapeEnd;
        if (glm::length(b - a) < 2.0f) b = a + glm::vec2(100.0f), a -= glm::vec2(0.0f); // (a click: a 100 px shape there)
        const bool square = io.KeyShift && !(m_ShapeMods & 1), centered = io.KeyAlt && !(m_ShapeMods & 2);
        switch (tool)
        {
        case Tool::RoundShape: s.Kind = ShapeKind::RoundedRectangle; break;
        case Tool::EllipseShape: s.Kind = ShapeKind::Ellipse; break;
        case Tool::PolygonShape: s.Kind = ShapeKind::Polygon; break;
        case Tool::StarShape: s.Kind = ShapeKind::Star; break;
        case Tool::LineShape: s.Kind = ShapeKind::Line; break;
        default: s.Kind = ShapeKind::Rectangle; break;
        }
        if (s.Kind == ShapeKind::Line)
        {
            glm::vec2 d = b - a;
            float angle = std::atan2(d.y, d.x);
            if (square) angle = std::round(angle / glm::quarter_pi<float>()) * glm::quarter_pi<float>(); // (45-degree steps)
            float len = glm::length(d);
            s.Center = a + glm::vec2(std::cos(angle), std::sin(angle)) * len * 0.5f;
            s.Size = { len, std::max(1.0f, m_ShapeStyle.Size.y) };
            s.Rotation = glm::degrees(angle);
        }
        else
        {
            if (square)
            {
                float side = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                b = a + glm::vec2(b.x >= a.x ? side : -side, b.y >= a.y ? side : -side);
            }
            if (centered) a = a - (b - a);
            s.Center = (a + b) * 0.5f;
            s.Size = glm::abs(b - a);
            s.Rotation = 0.0f;
        }
        Record(std::string("New ") + ShapeKindName(s.Kind));
        uint32_t parent = m_Doc.Layers.empty() ? 0 : m_Doc.Layers[(size_t)m_ActiveLayer].Parent;
        SetActive(m_Doc.AddShape(s, m_ActiveLayer + 1, parent));
        Changed();
        MarkAll();
    }

    void ImageMode::DrawVectorOverlay(ImDrawList* dl, const glm::vec2& mouse)
    {
        auto screen = [&](const glm::vec2& q) { return ImVec2(m_CanvasMin.x + q.x * m_Zoom, m_CanvasMin.y + q.y * m_Zoom); };
        auto box = [&](const std::array<glm::vec2, 4>& c, ImU32 col) {
            ImVec2 q[4] = { screen(c[0]), screen(c[1]), screen(c[2]), screen(c[3]) };
            dl->AddPolyline(q, 4, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 2.0f);
            dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, 1.0f);
        };
        // The text being typed: its box and the caret.
        if (m_TextEditing)
            if (int i = m_Doc.IndexOf(m_TextLayer); i >= 0)
            {
                const TextData& t = m_Doc.Layers[(size_t)i].Text;
                TextLayout layout = LayoutText(t);
                std::array<glm::vec2, 4> c = TextCorners(t, layout);
                if (t.Text.empty())
                {
                    // (an empty box would be a point: a short line where the text will go)
                    c = { TextToCanvas(t, { -1, -layout.Ascent }), TextToCanvas(t, { 1, -layout.Ascent }), TextToCanvas(t, { 1, layout.Descent }),
                          TextToCanvas(t, { -1, layout.Descent }) };
                }
                box(c, IM_COL32(120, 180, 255, 255));
                size_t line = 0;
                float x = 0.0f;
                if (CaretPlace(layout, m_Caret, line, x) && std::fmod(ImGui::GetTime(), 1.0) < 0.6)
                {
                    float base = layout.Lines[line].Baseline;
                    ImVec2 a = screen(TextToCanvas(t, { x, base - layout.Ascent })), b = screen(TextToCanvas(t, { x, base + layout.Descent }));
                    dl->AddLine(a, b, IM_COL32(0, 0, 0, 255), 3.0f);
                    dl->AddLine(a, b, IM_COL32(255, 255, 255, 255), 1.0f);
                }
            }
        // A shape being dragged out.
        if (m_ShapeDrag)
        {
            const ImGuiIO& io = ImGui::GetIO();
            glm::vec2 a = m_ShapeStart, b = m_ShapeEnd;
            if (m_Tool == Tool::LineShape)
            {
                dl->AddLine(screen(a), screen(b), IM_COL32(0, 0, 0, 200), 3.0f);
                dl->AddLine(screen(a), screen(b), IM_COL32(255, 255, 255, 255), 1.0f);
            }
            else
            {
                if (io.KeyShift && !(m_ShapeMods & 1))
                {
                    float side = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                    b = a + glm::vec2(b.x >= a.x ? side : -side, b.y >= a.y ? side : -side);
                }
                if (io.KeyAlt && !(m_ShapeMods & 2)) a = a - (b - a);
                ShapeData s = m_ShapeStyle;
                s.Kind = m_Tool == Tool::RoundShape     ? ShapeKind::RoundedRectangle
                         : m_Tool == Tool::EllipseShape ? ShapeKind::Ellipse
                         : m_Tool == Tool::PolygonShape ? ShapeKind::Polygon
                         : m_Tool == Tool::StarShape    ? ShapeKind::Star
                                                        : ShapeKind::Rectangle;
                s.Center = (a + b) * 0.5f;
                s.Size = glm::abs(b - a);
                std::vector<glm::vec2> outline = ShapeOutline(s);
                std::vector<ImVec2> pts;
                for (const glm::vec2& q : outline) pts.push_back(screen(q));
                dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 3.0f);
                dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 1.0f);
            }
        }
        // The active text or shape layer's box (with the Type, shape and Move tools).
        if (!m_TextEditing && !m_ShapeDrag && !m_Transform.Active && !m_Doc.Layers.empty() && (m_Tool == Tool::Type || IsShapeTool(m_Tool) || m_Tool == Tool::Move))
        {
            const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
            if (l.Kind == LayerKind::Shape) box(ShapeCorners(l.Shape), IM_COL32(120, 180, 255, 200));
            else if (l.Kind == LayerKind::Text) box(TextCorners(l.Text, LayoutText(l.Text)), IM_COL32(120, 180, 255, 200));
        }
        (void)mouse;
    }

    // ---- Settings

    bool ImageMode::DrawTextSettings(TextData& t, bool full)
    {
        bool changed = false;
        const auto& fonts = FindFonts(AssetManager::GetAssetsDirectory());
        std::string current = "Roboto (built in)";
        for (const FontFile& f : fonts)
            if (f.Path == t.Font) current = f.Name;
        if (!t.Font.empty() && current == "Roboto (built in)") current = fs::path(t.Font).stem().string() + " (missing)";
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo("##font", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##fontFilter", "Search fonts", &m_FontFilter);
            std::string filter = m_FontFilter;
            std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            for (const FontFile& f : fonts)
            {
                std::string lower = f.Name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                if (ImGui::Selectable(f.Name.c_str(), f.Path == t.Font)) t.Font = f.Path, changed = true;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The font (the system's and the project's .ttf/.otf files)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        changed |= ImGui::SliderFloat("##textSize", &t.Size, 4.0f, 1000.0f, "%.0f px", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Size");
        ImGui::SameLine();
        changed |= ImGui::ColorEdit4("##textColor", &t.Color.r, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine(0.0f, 12.0f);
        static const char* aligns[] = { "Left", "Center", "Right" };
        for (int a = 0; a < 3; ++a)
        {
            bool on = t.Align == a;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(aligns[a])) t.Align = a, changed = true;
            if (on) ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 2.0f);
        }
        ImGui::SameLine(0.0f, 12.0f);
        float leading = t.Leading * 100.0f;
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::SliderFloat("##leading", &leading, 50.0f, 300.0f, "%.0f%% lead", ImGuiSliderFlags_AlwaysClamp)) t.Leading = leading / 100.0f, changed = true;
        ImGui::SetItemTooltip("Line spacing");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat("##tracking", &t.Tracking, -20.0f, 100.0f, "%.1f track", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Extra space between letters (pixels)");
        ImGui::SameLine(0.0f, 12.0f);
        changed |= ImGui::Checkbox("Bold", &t.FauxBold);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Italic", &t.FauxItalic);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Smooth", &t.Antialias);
        ImGui::SetItemTooltip("Antialiased edges (off: hard pixels, for pixel art)");
        if (full)
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::InputTextMultiline("##textText", &t.Text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat2("Position", &t.Position.x, 1.0f, -100000.0f, 100000.0f, "%.0f");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::SliderFloat("Rotation", &t.Rotation, -180.0f, 180.0f, "%.1f deg");
        }
        return changed;
    }

    bool ImageMode::DrawShapeSettings(ShapeData& s, bool full)
    {
        bool changed = false;
        changed |= ImGui::Checkbox("Fill", &s.Fill);
        ImGui::SameLine();
        changed |= ImGui::ColorEdit4("##fillColor", &s.FillColor.r, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine(0.0f, 16.0f);
        changed |= ImGui::Checkbox("Stroke", &s.Stroke);
        ImGui::SameLine();
        changed |= ImGui::ColorEdit4("##strokeColor", &s.StrokeColor.r, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat("##strokeWidth", &s.StrokeWidth, 0.5f, 100.0f, "%.1f px", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Stroke width");
        ImGui::SameLine();
        static const char* aligns[] = { "Inside", "Center", "Outside" };
        ImGui::SetNextItemWidth(80.0f);
        changed |= ImGui::Combo("##strokeAlign", &s.StrokeAlign, aligns, 3);
        ImGui::SameLine(0.0f, 16.0f);
        const bool all = !full; // (the options bar sets every kind's settings for the next shape)
        if (all || s.Kind == ShapeKind::RoundedRectangle)
        {
            ImGui::SetNextItemWidth(90.0f);
            changed |= ImGui::SliderFloat("##radius", &s.Radius, 0.0f, 500.0f, "%.0f px radius", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Rounded rectangles: the corners' radius");
            ImGui::SameLine();
        }
        if (all || s.Kind == ShapeKind::Polygon || s.Kind == ShapeKind::Star)
        {
            ImGui::SetNextItemWidth(80.0f);
            changed |= ImGui::SliderInt("##sides", &s.Sides, 3, 64, "%d sides", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Polygons: sides; stars: points");
            ImGui::SameLine();
        }
        if (all || s.Kind == ShapeKind::Star)
        {
            float inset = s.Inset * 100.0f;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::SliderFloat("##inset", &inset, 0.0f, 95.0f, "%.0f%% indent", ImGuiSliderFlags_AlwaysClamp)) s.Inset = inset / 100.0f, changed = true;
            ImGui::SetItemTooltip("Stars: how deep the valleys go");
            ImGui::SameLine();
        }
        if (all || s.Kind == ShapeKind::Line)
        {
            ImGui::SetNextItemWidth(80.0f);
            changed |= ImGui::SliderFloat("##weight", &s.Size.y, 1.0f, 200.0f, "%.0f px weight", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Lines: their thickness");
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (full)
        {
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat2("Center", &s.Center.x, 1.0f, -100000.0f, 100000.0f, "%.0f");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat2(s.Kind == ShapeKind::Line ? "Length, Weight" : "Size", &s.Size.x, 1.0f, 1.0f, 100000.0f, "%.0f");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::SliderFloat("Rotation", &s.Rotation, -180.0f, 180.0f, "%.1f deg");
        }
        return changed;
    }

    void ImageMode::DrawTextOptions()
    {
        // The active text layer's settings (changing them changes it), else those for new text.
        const bool onLayer = !m_Doc.Layers.empty() && m_Doc.Layers[(size_t)m_ActiveLayer].Kind == LayerKind::Text;
        TextData t = onLayer ? m_Doc.Layers[(size_t)m_ActiveLayer].Text : m_TextStyle;
        if (DrawTextSettings(t, false))
        {
            m_TextStyle = t;
            m_TextStyle.Text = "Text";
            if (onLayer)
                EditVector(
                    [&](Layer& l) {
                        l.Text = t;
                        return true;
                    },
                    "Text Settings");
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (m_TextEditing)
        {
            if (ImGui::Button("Done")) CommitText();
            ImGui::SetItemTooltip("Esc or Ctrl+Enter");
        }
        else
            ImGui::TextDisabled("Click to type; click text to edit it");
    }

    void ImageMode::DrawShapeOptions()
    {
        const bool onLayer = !m_Doc.Layers.empty() && m_Doc.Layers[(size_t)m_ActiveLayer].Kind == LayerKind::Shape;
        ShapeData s = onLayer ? m_Doc.Layers[(size_t)m_ActiveLayer].Shape : m_ShapeStyle;
        if (onLayer)
        {
            // (the layer's own kind decides which settings show; new shapes keep the bar's kind-specific ones)
            ShapeData style = s;
            if (DrawShapeSettings(style, false))
            {
                m_ShapeStyle.Fill = style.Fill, m_ShapeStyle.FillColor = style.FillColor, m_ShapeStyle.Stroke = style.Stroke;
                m_ShapeStyle.StrokeColor = style.StrokeColor, m_ShapeStyle.StrokeWidth = style.StrokeWidth, m_ShapeStyle.StrokeAlign = style.StrokeAlign;
                m_ShapeStyle.Radius = style.Radius, m_ShapeStyle.Sides = style.Sides, m_ShapeStyle.Inset = style.Inset;
                EditVector(
                    [&](Layer& l) {
                        l.Shape = style;
                        return true;
                    },
                    "Shape Settings");
            }
        }
        else
            DrawShapeSettings(m_ShapeStyle, false);
    }

    void ImageMode::DrawVectorProperties()
    {
        Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        ImGui::TextColored(UI::AccentText(), "%s", l.Name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(l.Kind == LayerKind::Text ? "(text)" : "(%s)", ShapeKindName(l.Shape.Kind));
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        if (l.Kind == LayerKind::Text)
        {
            TextData t = l.Text;
            if (DrawTextSettings(t, true))
                EditVector(
                    [&](Layer& layer) {
                        layer.Text = t;
                        return true;
                    },
                    "Text Settings");
        }
        else
        {
            ShapeData s = l.Shape;
            if (DrawShapeSettings(s, true))
                EditVector(
                    [&](Layer& layer) {
                        layer.Shape = s;
                        return true;
                    },
                    "Shape Settings");
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Layer Style...")) OpenLayerStyle();
        ImGui::SameLine();
        if (ImGui::Button("Rasterize")) RasterizeLayer();
        ImGui::SetItemTooltip("Make it a pixel layer to paint on (its text or shape settings go)");
    }
}
