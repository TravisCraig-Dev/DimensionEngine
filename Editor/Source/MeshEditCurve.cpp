#include "MeshEditMode.h"
#include "EditorUI.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Scene/Components.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Edit Mode's curves and text, and retopology helpers (snapping to other objects' surfaces).
namespace Editor
{
    using namespace Dimension::Modeling;
    using MCurve = Dimension::Modeling::Curve;

    namespace
    {
        constexpr ImU32 kCurveLine = IM_COL32(230, 230, 230, 230);
        constexpr ImU32 kCurveSelected = IM_COL32(255, 160, 40, 255);
        constexpr ImU32 kCurveHull = IM_COL32(150, 150, 150, 120);

        float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

        bool RayPlane(const glm::vec3& from, const glm::vec3& dir, const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
        {
            float d = glm::dot(dir, normal);
            if (std::abs(d) < 1e-8f) return false;
            hit = from + dir * (glm::dot(point - from, normal) / d);
            return true;
        }
    }

    // ---- Modes

    void MeshEditMode::SetCurveMode(bool on)
    {
        if (!m_Active || on == m_CurveMode) return;
        if (m_Modal.Kind != ModalKind::None) EndModal(false);
        if (m_CurveModal.Kind) EndCurveModal(false);
        if (on)
        {
            if (m_Sculpting) SetSculpting(false);
            if (m_PaintMode) SetPaintMode(0);
            if (m_RigMode) SetRigMode(0);
        }
        m_CurveMode = on;
        m_Pressing = m_BoxSelecting = false;
        ++m_GeometryVersion;
    }

    void MeshEditMode::ConvertGeneratedToMesh()
    {
        if (!m_Mesh.HasGenerated()) return;
        Record("Convert to Mesh");
        m_Mesh = m_Mesh.WithGenerated();
        Ops::SelectAll(m_Mesh, m_Mode);
        m_CurveMode = false;
        ++m_GeometryVersion;
        Commit();
        Log::Info("Converted to a mesh: {} vertices, {} faces", m_Mesh.VertexCount(), m_Mesh.FaceCount());
    }

    // ---- Curve points on screen

    bool MeshEditMode::CurvePointScreen(size_t c, size_t p, ImVec2& out) const
    {
        glm::vec4 clip = m_ViewProj * m_World * glm::vec4(m_Mesh.Curves[c].Points[p].Position, 1.0f);
        if (clip.w <= 1e-6f) return false;
        out = ImVec2(m_Origin.x + (clip.x / clip.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - clip.y / clip.w * 0.5f) * m_Size.y);
        return true;
    }

    void MeshEditMode::DrawCurveOverlay(ImDrawList* dl)
    {
        glm::mat4 mvp = m_ViewProj * m_World;
        auto project = [&](const glm::vec3& p, ImVec2& out) {
            glm::vec4 c = mvp * glm::vec4(p, 1.0f);
            if (c.w <= 1e-6f) return false;
            out = ImVec2(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
            return true;
        };
        for (size_t c = 0; c < m_Mesh.Curves.size(); ++c)
        {
            const MCurve& curve = m_Mesh.Curves[c];
            bool any = false;
            for (const CurvePoint& p : curve.Points) any |= (p.Flags & Selected) != 0;
            // The curve itself, then the control polygon and points.
            std::vector<glm::vec3> samples = CurveOps::Sample(curve, std::max(m_Mesh.CurveOptions.Resolution, 4));
            if (curve.Cyclic && curve.Points.size() >= 3 && !samples.empty()) samples.push_back(samples.front());
            ImVec2 prev;
            bool havePrev = false;
            for (const glm::vec3& s : samples)
            {
                ImVec2 q;
                bool ok = project(s, q);
                if (ok && havePrev) dl->AddLine(prev, q, any ? kCurveSelected : kCurveLine, 2.0f);
                prev = q;
                havePrev = ok;
            }
            for (size_t i = 0; i + 1 < curve.Points.size() + (curve.Cyclic ? 1 : 0) && curve.Points.size() > 1; ++i)
            {
                ImVec2 a, b;
                if (CurvePointScreen(c, i, a) && CurvePointScreen(c, (i + 1) % curve.Points.size(), b)) dl->AddLine(a, b, kCurveHull, 1.0f);
            }
            // Handles of selected points (Auto ones drawn dimmer).
            for (size_t i = 0; i < curve.Points.size(); ++i)
            {
                if (!(curve.Points[i].Flags & Selected) || curve.Points[i].Sharp) continue;
                glm::vec3 hin, hout;
                CurveOps::Handles(curve, i, hin, hout);
                ImVec2 a, hi, ho;
                if (!CurvePointScreen(c, i, a) || !project(hin, hi) || !project(hout, ho)) continue;
                ImU32 col = curve.Points[i].HandleType == 0 ? IM_COL32(200, 200, 120, 160) : curve.Points[i].HandleType == 1 ? IM_COL32(230, 120, 230, 230) : IM_COL32(120, 230, 120, 230);
                dl->AddLine(hi, a, col, 1.0f);
                dl->AddLine(a, ho, col, 1.0f);
                dl->AddCircle(hi, 4.0f, col, 12, 1.5f);
                dl->AddCircle(ho, 4.0f, col, 12, 1.5f);
            }
            for (size_t i = 0; i < curve.Points.size(); ++i)
            {
                ImVec2 a;
                if (!CurvePointScreen(c, i, a)) continue;
                bool sel = curve.Points[i].Flags & Selected;
                if (curve.Points[i].Sharp) dl->AddRectFilled(ImVec2(a.x - 4, a.y - 4), ImVec2(a.x + 4, a.y + 4), sel ? kCurveSelected : IM_COL32(30, 30, 30, 255));
                else dl->AddCircleFilled(a, 4.5f, sel ? kCurveSelected : IM_COL32(30, 30, 30, 255));
                dl->AddCircle(a, 4.5f, IM_COL32(255, 255, 255, 120));
            }
        }
    }

    // ---- Moving points (G) and their radius (Alt+S)

    void MeshEditMode::BeginCurveModal(int kind, bool recorded)
    {
        glm::vec3 sum(0.0f);
        int count = 0;
        for (const MCurve& c : m_Mesh.Curves)
            for (const CurvePoint& p : c.Points)
                if (p.Flags & Selected)
                {
                    sum += p.Position;
                    ++count;
                }
        if (count == 0) return;
        if (!recorded) Record(kind == 1 ? "Move Curve Points" : "Curve Radius");
        m_CurveModal = {};
        m_CurveModal.Kind = kind;
        m_CurveModal.Recorded = recorded;
        m_CurveModal.Start = m_Mesh.Curves;
        m_CurveModal.Pivot = sum / (float)count;
        m_CurveModal.MouseStart = ImGui::GetIO().MousePos;
        m_CurveModal.StartFrame = ImGui::GetFrameCount();
        glm::vec3 from, dir;
        SculptRay(m_CurveModal.MouseStart, from, dir);
        RayPlane(from, dir, m_CurveModal.Pivot, glm::normalize(m_CurveModal.Pivot - m_CameraLocal), m_CurveModal.PlaneStart);
    }

    void MeshEditMode::UpdateCurveModal()
    {
        CurveModal& md = m_CurveModal;
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        for (int a = 0; a < 3; ++a)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_X + a), false)) md.Axis = md.Axis == a ? -1 : a;
        m_Mesh.Curves = md.Start;
        if (md.Kind == 3)
        {
            // A handle dragged: Auto handles become Aligned (kept in line); Aligned turns the other one too.
            glm::vec3 from, dir, hit, move(0.0f);
            SculptRay(mouse, from, dir);
            if (RayPlane(from, dir, md.Pivot, glm::normalize(md.Pivot - m_CameraLocal), hit)) move = hit - md.PlaneStart;
            if (md.Axis >= 0)
            {
                float along = move[md.Axis];
                move = glm::vec3(0.0f);
                move[md.Axis] = along;
            }
            CurvePoint& p = m_Mesh.Curves[md.HandleCurve].Points[md.HandlePoint];
            glm::vec3 hin, hout;
            CurveOps::Handles(md.Start[md.HandleCurve], md.HandlePoint, hin, hout);
            if (p.HandleType == 0)
            {
                p.HandleType = 1;
                p.HandleIn = hin - p.Position;
                p.HandleOut = hout - p.Position;
            }
            glm::vec3& moved = md.HandleSide == 1 ? p.HandleIn : p.HandleOut;
            glm::vec3& other = md.HandleSide == 1 ? p.HandleOut : p.HandleIn;
            moved = md.HandleStart + move - p.Position;
            if (p.HandleType == 1 && glm::length(moved) > 1e-9f) other = -glm::normalize(moved) * glm::length(other);
        }
        else if (md.Kind == 1)
        {
            glm::vec3 from, dir, hit, move(0.0f);
            SculptRay(mouse, from, dir);
            if (RayPlane(from, dir, md.Pivot, glm::normalize(md.Pivot - m_CameraLocal), hit)) move = hit - md.PlaneStart;
            if (md.Axis >= 0)
            {
                float along = move[md.Axis];
                move = glm::vec3(0.0f);
                move[md.Axis] = along;
            }
            if (io.KeyCtrl) move = glm::round(move * 10.0f) / 10.0f;
            for (MCurve& c : m_Mesh.Curves)
                for (CurvePoint& p : c.Points)
                    if (p.Flags & Selected)
                    {
                        p.Position += move;
                        // Retopology: points dropped onto the surface under them.
                        if (m_SnapSurface)
                        {
                            glm::vec3 world = glm::vec3(m_World * glm::vec4(p.Position, 1.0f)), hitW, n;
                            if (SnapRay(m_CameraWorld, glm::normalize(world - m_CameraWorld), hitW, n))
                                p.Position = glm::vec3(glm::inverse(m_World) * glm::vec4(hitW + n * m_SnapOffset, 1.0f));
                        }
                    }
        }
        else
        {
            glm::vec4 c = m_ViewProj * m_World * glm::vec4(md.Pivot, 1.0f);
            ImVec2 ps(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
            float scale = Dist(mouse, ps) / std::max(Dist(md.MouseStart, ps), 1.0f);
            for (MCurve& cv : m_Mesh.Curves)
                for (CurvePoint& p : cv.Points)
                    if (p.Flags & Selected) p.Radius = std::max(0.0f, p.Radius * scale);
        }
        Commit();
        bool later = ImGui::GetFrameCount() > md.StartFrame;
        bool dragEnded = md.Kind == 3 && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (dragEnded || (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))))
            EndCurveModal(true);
        else if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
            EndCurveModal(false);
    }

    void MeshEditMode::EndCurveModal(bool confirm)
    {
        if (!m_CurveModal.Kind) return;
        if (!confirm)
        {
            if (m_CurveModal.Recorded && !m_Undo.empty()) m_Mesh = std::move(m_Undo.back().Mesh);
            else m_Mesh.Curves = m_CurveModal.Start;
            if (!m_Undo.empty()) m_Undo.pop_back();
            Commit();
        }
        m_CurveModal = {};
    }

    void MeshEditMode::AddCurvePointAtMouse(const ImVec2& mouse)
    {
        // After the selected end point (or before a selected first point), else a new curve; on the view plane through
        // that point (or the surface under the mouse when snapping).
        int curve = -1, at = -1;
        bool atStart = false;
        for (size_t c = 0; c < m_Mesh.Curves.size() && curve < 0; ++c)
        {
            auto& pts = m_Mesh.Curves[c].Points;
            if (pts.empty() || m_Mesh.Curves[c].Cyclic) continue;
            if (pts.back().Flags & Selected) { curve = (int)c; at = (int)pts.size() - 1; }
            else if (pts.front().Flags & Selected) { curve = (int)c; at = 0; atStart = true; }
        }
        glm::vec3 anchor = curve >= 0 ? m_Mesh.Curves[(size_t)curve].Points[(size_t)at].Position : glm::vec3(0.0f);
        glm::vec3 from, dir, p;
        SculptRay(mouse, from, dir);
        if (!RayPlane(from, dir, anchor, glm::normalize(anchor - m_CameraLocal), p)) return;
        if (m_SnapSurface)
        {
            glm::vec3 world = glm::vec3(m_World * glm::vec4(from, 1.0f)), wdir = glm::normalize(glm::vec3(m_World * glm::vec4(dir, 0.0f))), hit, n;
            if (SnapRay(world, wdir, hit, n)) p = glm::vec3(glm::inverse(m_World) * glm::vec4(hit + n * m_SnapOffset, 1.0f));
        }
        Record("Add Curve Point");
        for (MCurve& c : m_Mesh.Curves)
            for (CurvePoint& q : c.Points) q.Flags = 0;
        CurvePoint np;
        np.Position = p;
        np.Flags = Selected;
        if (curve < 0)
        {
            MCurve c;
            c.Points.push_back(np);
            m_Mesh.Curves.push_back(c);
        }
        else
        {
            auto& pts = m_Mesh.Curves[(size_t)curve].Points;
            np.Radius = pts[(size_t)at].Radius;
            if (atStart) pts.insert(pts.begin(), np);
            else pts.push_back(np);
        }
        Commit();
    }

    // ---- Keys and mouse

    void MeshEditMode::HandleCurveKeys()
    {
        using Shortcuts::Pressed;
        auto op = [&](const char* name, auto fn) {
            Record(name);
            fn();
            Commit();
        };
        if (Pressed("curve.selectAll"))
            op("Select All Points", [&] {
                for (MCurve& c : m_Mesh.Curves)
                    for (CurvePoint& p : c.Points) p.Flags |= Selected;
            });
        if (Pressed("curve.deselectAll"))
            op("Select No Points", [&] {
                for (MCurve& c : m_Mesh.Curves)
                    for (CurvePoint& p : c.Points) p.Flags = 0;
            });
        if (Pressed("curve.grab")) BeginCurveModal(1);
        if (Pressed("curve.radius")) BeginCurveModal(2);
        if (Pressed("curve.extrude"))
        {
            // A new point after each selected end point, then moved with the mouse.
            bool any = false;
            for (const MCurve& c : m_Mesh.Curves)
                if (!c.Cyclic && !c.Points.empty()) any |= (c.Points.back().Flags & Selected) || (c.Points.front().Flags & Selected);
            if (any)
            {
                Record("Extrude Curve");
                for (MCurve& c : m_Mesh.Curves)
                {
                    if (c.Cyclic || c.Points.empty()) continue;
                    bool end = c.Points.back().Flags & Selected, start = c.Points.front().Flags & Selected && c.Points.size() > 1;
                    for (CurvePoint& p : c.Points) p.Flags = 0;
                    if (end)
                    {
                        CurvePoint np = c.Points.back();
                        np.Flags = Selected;
                        c.Points.push_back(np);
                    }
                    if (start)
                    {
                        CurvePoint np = c.Points.front();
                        np.Flags = Selected;
                        c.Points.insert(c.Points.begin(), np);
                    }
                }
                BeginCurveModal(1, true);
            }
        }
        if (Pressed("curve.delete"))
            op("Delete Curve Points", [&] {
                for (MCurve& c : m_Mesh.Curves)
                    c.Points.erase(std::remove_if(c.Points.begin(), c.Points.end(), [](const CurvePoint& p) { return (p.Flags & Selected) != 0; }), c.Points.end());
                m_Mesh.Curves.erase(std::remove_if(m_Mesh.Curves.begin(), m_Mesh.Curves.end(), [](const MCurve& c) { return c.Points.empty(); }), m_Mesh.Curves.end());
            });
        if (Pressed("curve.cyclic"))
            op("Toggle Cyclic", [&] {
                for (MCurve& c : m_Mesh.Curves)
                {
                    bool any = false;
                    for (const CurvePoint& p : c.Points) any |= (p.Flags & Selected) != 0;
                    if (any) c.Cyclic = !c.Cyclic;
                }
            });
        if (Pressed("curve.handleType"))
            op("Handle Type", [&] {
                // Auto -> Aligned -> Free -> Auto, keeping the handles where they are.
                for (MCurve& c : m_Mesh.Curves)
                    for (size_t i = 0; i < c.Points.size(); ++i)
                    {
                        CurvePoint& p = c.Points[i];
                        if (!(p.Flags & Selected)) continue;
                        glm::vec3 hin, hout;
                        CurveOps::Handles(c, i, hin, hout);
                        p.HandleIn = hin - p.Position;
                        p.HandleOut = hout - p.Position;
                        p.HandleType = (uint8_t)((p.HandleType + 1) % 3);
                        p.Sharp = false;
                    }
            });
        if (Pressed("curve.sharp"))
            op("Toggle Sharp", [&] {
                for (MCurve& c : m_Mesh.Curves)
                    for (CurvePoint& p : c.Points)
                        if (p.Flags & Selected) p.Sharp = !p.Sharp;
            });
        if (Pressed("curve.add"))
            op("Add Curve", [&] {
                for (MCurve& c : m_Mesh.Curves)
                    for (CurvePoint& p : c.Points) p.Flags = 0;
                MCurve c = CurveOps::Make({ { -0.5f, 0, 0 }, { 0, 0.3f, 0 }, { 0.5f, 0, 0 } }, false);
                for (CurvePoint& p : c.Points) p.Flags = Selected;
                m_Mesh.Curves.push_back(c);
            });
    }

    void MeshEditMode::HandleCurveMouse(bool hovered, bool blocked)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        bool overToolbar = mouse.x >= m_ToolbarMin.x && mouse.x <= m_ToolbarMax.x && mouse.y >= m_ToolbarMin.y && mouse.y <= m_ToolbarMax.y;
        if (!(hovered && !blocked && !m_OverUI && !overToolbar && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return;
        if (io.KeyCtrl)
        {
            AddCurvePointAtMouse(mouse);
            return;
        }
        // A selected point's handle: drag it.
        glm::mat4 mvp = m_ViewProj * m_World;
        for (size_t c = 0; c < m_Mesh.Curves.size(); ++c)
            for (size_t p = 0; p < m_Mesh.Curves[c].Points.size(); ++p)
            {
                const CurvePoint& cp = m_Mesh.Curves[c].Points[p];
                if (!(cp.Flags & Selected) || cp.Sharp) continue;
                glm::vec3 hs[2];
                CurveOps::Handles(m_Mesh.Curves[c], p, hs[0], hs[1]);
                for (int side = 0; side < 2; ++side)
                {
                    glm::vec4 clip = mvp * glm::vec4(hs[side], 1.0f);
                    if (clip.w <= 1e-6f) continue;
                    ImVec2 s(m_Origin.x + (clip.x / clip.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - clip.y / clip.w * 0.5f) * m_Size.y);
                    if (Dist(s, mouse) > 7.0f) continue;
                    Record("Move Curve Handle");
                    m_CurveModal = {};
                    m_CurveModal.Kind = 3;
                    m_CurveModal.Recorded = true;
                    m_CurveModal.Start = m_Mesh.Curves;
                    m_CurveModal.HandleCurve = c;
                    m_CurveModal.HandlePoint = p;
                    m_CurveModal.HandleSide = side + 1;
                    m_CurveModal.HandleStart = hs[side];
                    m_CurveModal.Pivot = hs[side];
                    m_CurveModal.MouseStart = mouse;
                    m_CurveModal.StartFrame = ImGui::GetFrameCount() - 1; // (the release ends it: a drag)
                    glm::vec3 from, dir;
                    SculptRay(mouse, from, dir);
                    RayPlane(from, dir, hs[side], glm::normalize(hs[side] - m_CameraLocal), m_CurveModal.PlaneStart);
                    return;
                }
            }
        float best = 12.0f;
        int bc = -1, bp = -1;
        for (size_t c = 0; c < m_Mesh.Curves.size(); ++c)
            for (size_t p = 0; p < m_Mesh.Curves[c].Points.size(); ++p)
            {
                ImVec2 s;
                if (CurvePointScreen(c, p, s) && Dist(s, mouse) < best)
                {
                    best = Dist(s, mouse);
                    bc = (int)c;
                    bp = (int)p;
                }
            }
        Record("Select Curve Point");
        if (!io.KeyShift)
            for (MCurve& c : m_Mesh.Curves)
                for (CurvePoint& p : c.Points) p.Flags = 0;
        if (bc >= 0)
        {
            CurvePoint& p = m_Mesh.Curves[(size_t)bc].Points[(size_t)bp];
            p.Flags = io.KeyShift ? (uint8_t)(p.Flags ^ Selected) : (uint8_t)Selected;
        }
        m_Dirty = true;
    }

    bool MeshEditMode::UpdateCurveView(bool hovered, bool focused, bool blocked)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawCurveOverlay(dl);
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        DrawCurveToolbar(m_Origin);
        ImGui::SetCursorScreenPos(cursor);
        m_OverUI = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (m_CurveModal.Kind) UpdateCurveModal();
        else
        {
            if ((hovered || focused) && !ImGui::GetIO().WantTextInput) HandleCurveKeys();
            if (m_CurveMode && !m_CurveModal.Kind) HandleCurveMouse(hovered, blocked);
        }
        size_t points = 0;
        for (const MCurve& c : m_Mesh.Curves) points += c.Points.size();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Curves: %zu (%zu points)   click a point (Shift adds)   Ctrl+click: add a point   G move   E extrude   %s radius   %s cyclic   %s sharp   X delete",
                      m_Mesh.Curves.size(), points, Shortcuts::Label("curve.radius").c_str(), Shortcuts::Label("curve.cyclic").c_str(), Shortcuts::Label("curve.sharp").c_str());
        std::string status = buf;
        if (m_CurveModal.Kind)
            status = std::string(m_CurveModal.Kind == 1 ? "Move" : m_CurveModal.Kind == 3 ? "Handle" : "Radius") + (m_CurveModal.Axis >= 0 ? std::string("  along ") + "XYZ"[m_CurveModal.Axis] : "") +
                     "   X/Y/Z axis, Ctrl snap; click or Enter: done, right-click or Esc: cancel";
        dl->AddText(ImVec2(m_Origin.x + 8.0f, m_Origin.y + m_Size.y - 24.0f), IM_COL32(255, 255, 255, 230), status.c_str());
        return m_CurveModal.Kind != 0;
    }

    void MeshEditMode::DrawCurveToolbar(const ImVec2& origin)
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 28.0f));
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
        if (ImGui::Button("Edit Mesh")) SetCurveMode(false);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The model's own vertices, edges and faces");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::Button("Curves");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Add Curve")) Shortcuts::Press("curve.add");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Cyclic")) Shortcuts::Press("curve.cyclic");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Closes (or opens) the curves with selected points (%s)", Shortcuts::Label("curve.cyclic").c_str());
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Sharp")) Shortcuts::Press("curve.sharp");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Selected points become corners (or smooth again) (%s)", Shortcuts::Label("curve.sharp").c_str());
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Delete")) Shortcuts::Press("curve.delete");
        ImGui::SameLine();
        if (ImGui::Button("Text...")) ImGui::OpenPopup("TextSettings");
        DrawTextPopup();
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Convert to Mesh")) ConvertGeneratedToMesh();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns the curves and text into the model's own faces (editable vertex by vertex; can't be edited as curves after)");
        ImGui::SameLine();
        if (ImGui::Button("Done")) Exit();

        // Second row: how curves become geometry, and the selected points.
        CurveSettings& s = m_Mesh.CurveOptions;
        auto setting = [&](bool changed) {
            if (ImGui::IsItemActivated()) Record("Curve Settings");
            if (changed) Commit();
        };
        ImGui::SetNextItemWidth(80.0f);
        setting(ImGui::DragFloat("##bevel", &s.BevelDepth, 0.002f, 0.0f, 100.0f, "bevel %.3f"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tube radius (0: a ribbon with Extrude, or just lines)");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(70.0f);
        setting(ImGui::SliderInt("##bevelres", &s.BevelResolution, 3, 32, "%d sides"));
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(70.0f);
        setting(ImGui::SliderInt("##res", &s.Resolution, 1, 64, "res %d"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Steps between control points");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(80.0f);
        setting(ImGui::DragFloat("##extrude", &s.Extrude, 0.002f, 0.0f, 100.0f, "extrude %.3f"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A ribbon's half width, or a filled shape's half thickness");
        ImGui::SameLine();
        bool fill = s.Fill, caps = s.Caps, smooth = s.Smooth;
        if (ImGui::Checkbox("Fill", &fill)) { Record("Curve Fill"); s.Fill = fill; Commit(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Closed curves filled flat in the XY plane (curves inside others make holes)");
        ImGui::SameLine();
        if (ImGui::Checkbox("Caps", &caps)) { Record("Curve Caps"); s.Caps = caps; Commit(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Smooth", &smooth)) { Record("Curve Smooth"); s.Smooth = smooth; Commit(); }
        // The selected points' radius and tilt (the first selected one's shown).
        CurvePoint* first = nullptr;
        for (MCurve& c : m_Mesh.Curves)
            for (CurvePoint& p : c.Points)
                if (!first && (p.Flags & Selected)) first = &p;
        if (first)
        {
            ImGui::SameLine();
            float radius = first->Radius, tilt = glm::degrees(first->Tilt);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragFloat("##pradius", &radius, 0.01f, 0.0f, 100.0f, "radius %.2f"))
            {
                if (ImGui::IsItemActivated()) Record("Curve Radius");
                for (MCurve& c : m_Mesh.Curves)
                    for (CurvePoint& p : c.Points)
                        if (p.Flags & Selected) p.Radius = radius;
                Commit();
            }
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragFloat("##ptilt", &tilt, 0.5f, -720.0f, 720.0f, "tilt %.0f"))
            {
                if (ImGui::IsItemActivated()) Record("Curve Tilt");
                for (MCurve& c : m_Mesh.Curves)
                    for (CurvePoint& p : c.Points)
                        if (p.Flags & Selected) p.Tilt = glm::radians(tilt);
                Commit();
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        m_ToolbarMin = ImGui::GetItemRectMin();
        m_ToolbarMax = ImGui::GetItemRectMax();
    }

    // ---- Text

    void MeshEditMode::DrawTextPopup()
    {
        if (!ImGui::BeginPopup("TextSettings")) return;
        TextSettings& t = m_Mesh.Text;
        bool enabled = t.Enabled;
        if (ImGui::Checkbox("Text", &enabled))
        {
            Record("Text");
            t.Enabled = enabled;
            if (enabled && t.Body.empty()) t.Body = "Text";
            Commit();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solid letters (in the model's XY plane, facing +Z) made with the model");
        if (t.Enabled)
        {
            // The text box edits a copy, refreshed from the model while it isn't being typed in (undo, other edits).
            static char buffer[2048];
            static bool typing = false;
            if (!typing) std::snprintf(buffer, sizeof(buffer), "%s", t.Body.c_str());
            bool changed = ImGui::InputTextMultiline("##body", buffer, sizeof(buffer), ImVec2(260.0f, 70.0f));
            if (ImGui::IsItemActivated()) Record("Edit Text");
            typing = ImGui::IsItemActive();
            if (changed)
            {
                t.Body = buffer;
                Commit();
            }
            if (UI::BeginProperties("##textprops"))
            {
                AssetHandle font(t.Font);
                if (UI::AssetField("Font", font, AssetType::Font).Changed)
                {
                    Record("Text Font");
                    t.Font = (uint64_t)font;
                    Commit();
                }
                auto field = [&](const char* label, const char* tip, auto draw) {
                    UI::PropertyLabel(label, tip);
                    bool changed = draw();
                    if (ImGui::IsItemActivated()) Record("Text Settings");
                    if (changed) Commit();
                };
                field("Size", "Line height", [&] { return ImGui::DragFloat("##size", &t.Size, 0.01f, 0.001f, 1000.0f, "%.3f"); });
                field("Depth", "Thickness (0: flat)", [&] { return ImGui::DragFloat("##depth", &t.Depth, 0.005f, 0.0f, 100.0f, "%.3f"); });
                field("Bevel", "The letters' front and back edges chamfered this much (up to half the depth)",
                      [&] { return ImGui::DragFloat("##bevel", &t.Bevel, 0.001f, 0.0f, 10.0f, "%.3f"); });
                field("Spacing", "Extra space between letters (times the size)", [&] { return ImGui::DragFloat("##spacing", &t.Spacing, 0.005f, -1.0f, 5.0f, "%.3f"); });
                field("Line Spacing", nullptr, [&] { return ImGui::DragFloat("##lines", &t.LineSpacing, 0.01f, 0.1f, 10.0f, "%.2f"); });
                field("Align", nullptr, [&] {
                    const char* aligns[] = { "Left", "Center", "Right" };
                    return ImGui::Combo("##align", &t.Align, aligns, 3);
                });
                field("Resolution", "Steps along each curved part of the letters", [&] { return ImGui::SliderInt("##res", &t.Resolution, 1, 16); });
                UI::EndProperties();
            }
            if (ImGui::Button("Convert to Mesh")) ConvertGeneratedToMesh();
        }
        ImGui::EndPopup();
    }

    // ---- Snapping to surfaces (retopology)

    void MeshEditMode::BuildSnapTree()
    {
        // Every other object with a mesh, in world space (built again when any of them moves or changes mesh).
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        uint64_t sig = 1469598103934665603ull;
        auto mix = [&](const void* data, size_t n) {
            const uint8_t* b = (const uint8_t*)data;
            for (size_t i = 0; i < n; ++i) sig = (sig ^ b[i]) * 1099511628211ull;
        };
        for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
        {
            if (e == m_Entity || !mr.Mesh.IsValid()) continue;
            uint64_t h = (uint64_t)mr.Mesh;
            mix(&h, sizeof(h));
            mix(&t.World, sizeof(t.World));
            auto data = AssetManager::GetMeshData(mr.Mesh);
            size_t n = data ? data->Vertices.size() : 0;
            mix(&n, sizeof(n));
        }
        if (sig == m_SnapSignature && !m_SnapTree.Empty()) return;
        m_SnapSignature = sig;
        m_SnapTree = TriangleTree();
        std::vector<glm::vec3> positions;
        for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
        {
            if (e == m_Entity || !mr.Mesh.IsValid()) continue;
            auto data = AssetManager::GetMeshData(mr.Mesh);
            if (!data) continue;
            positions.clear();
            for (const Vertex& v : data->Vertices) positions.push_back(v.Position);
            m_SnapTree.Add(positions, data->Indices, t.World);
        }
        m_SnapTree.Finish();
    }

    bool MeshEditMode::SnapRay(const glm::vec3& from, const glm::vec3& dir, glm::vec3& hit, glm::vec3& normal)
    {
        BuildSnapTree();
        TriangleTree::Hit h = m_SnapTree.Raycast(from, dir);
        if (!h.Found) return false;
        hit = h.Point;
        normal = glm::dot(h.Normal, dir) > 0.0f ? -h.Normal : h.Normal; // (the side the ray came from)
        return true;
    }

    void MeshEditMode::SnapModalVertices()
    {
        // The moved vertices dropped onto the surface seen behind them from the camera, or the nearest surface point.
        glm::mat4 inv = glm::inverse(m_World);
        BuildSnapTree();
        for (uint32_t v = 0; v < m_Mesh.VertexCount() && v < m_Modal.Weights.size(); ++v)
        {
            if (m_Modal.Weights[v] <= 0.0f) continue;
            glm::vec3 world = glm::vec3(m_World * glm::vec4(m_Mesh.Positions[v], 1.0f)), hit, n;
            if (m_SnapMode == 1)
            {
                TriangleTree::Hit h = m_SnapTree.Closest(world);
                if (!h.Found) continue;
                // (the normal's side facing the vertex: out of the surface it was moved over)
                n = glm::dot(h.Normal, world - h.Point) < 0.0f && h.Distance > 1e-6f ? -h.Normal : h.Normal;
                m_Mesh.Positions[v] = glm::vec3(inv * glm::vec4(h.Point + n * m_SnapOffset, 1.0f));
                continue;
            }
            if (SnapRay(m_CameraWorld, glm::normalize(world - m_CameraWorld), hit, n)) m_Mesh.Positions[v] = glm::vec3(inv * glm::vec4(hit + n * m_SnapOffset, 1.0f));
        }
    }

    void MeshEditMode::AddVertexAtMouse(const ImVec2& mouse)
    {
        // Ctrl+click: a new vertex at the surface under the mouse (snapping on) or on the view plane through the
        // selection, joined by an edge to the vertex selected before (Blender's Ctrl+right-click).
        std::vector<uint32_t> selected = Ops::SelectedVertices(m_Mesh);
        glm::vec3 anchor = selected.empty() ? glm::vec3(0.0f) : Ops::SelectionCenter(m_Mesh);
        glm::vec3 from, dir, p;
        SculptRay(mouse, from, dir);
        if (!RayPlane(from, dir, anchor, glm::normalize(anchor - m_CameraLocal), p)) return;
        if (m_SnapSurface)
        {
            glm::vec3 world = glm::vec3(m_World * glm::vec4(from, 1.0f)), wdir = glm::normalize(glm::vec3(m_World * glm::vec4(dir, 0.0f))), hit, n;
            if (SnapRay(world, wdir, hit, n)) p = glm::vec3(glm::inverse(m_World) * glm::vec4(hit + n * m_SnapOffset, 1.0f));
        }
        Record("Add Vertex");
        uint32_t v = m_Mesh.AddVertex(p);
        if (selected.size() == 1) m_Mesh.AddLooseEdge(selected[0], v);
        Ops::DeselectAll(m_Mesh);
        Ops::SelectVertex(m_Mesh, v, true);
        m_Mesh.Changed();
        Ops::FlushSelection(m_Mesh, m_Mode);
        Commit();
    }
}
