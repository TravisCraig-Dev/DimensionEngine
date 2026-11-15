#include "MeshEditMode.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Scene/Components.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Edit Mode's Sculpt Mode: brushes on the model being edited.
namespace Editor
{
    using namespace Dimension::Modeling;
    namespace sc = Dimension::Modeling::Sculpt;

    namespace
    {
        float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

        bool RayPlane(const glm::vec3& from, const glm::vec3& dir, const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
        {
            float d = glm::dot(dir, normal);
            if (std::abs(d) < 1e-8f) return false;
            hit = from + dir * (glm::dot(point - from, normal) / d);
            return true;
        }

        const char* kBrushKeys[] = { "sculpt.draw", "sculpt.clay", "sculpt.inflate", "sculpt.smooth", "sculpt.flatten", "sculpt.scrape",
                                     "sculpt.pinch", "sculpt.crease", "sculpt.grab", "sculpt.snakeHook", "sculpt.mask" };
        const char* kBrushTips[] = { "Pushes the surface out (in with Ctrl or Subtract)",
                                     "Builds up towards a plane above the surface, filling dips first",
                                     "Each part swells along its own normal (shrinks with Ctrl)",
                                     "Evens out bumps (Shift with any brush)",
                                     "Pulls towards the plane under the brush",
                                     "Cuts down what stands above the plane under the brush (fills dips with Ctrl)",
                                     "Pulls towards the brush's center (pushes apart with Ctrl)",
                                     "A sharp groove (a ridge with Ctrl)",
                                     "Drags the part caught under the brush with the mouse",
                                     "Pulls the surface along the stroke (with dynamic topology, long new shapes)",
                                     "Paints the mask: masked parts don't move (Ctrl erases)" };
    }

    void MeshEditMode::SetSculpting(bool on)
    {
        if (!m_Active || on == m_Sculpting) return;
        if (on && m_PaintMode) SetPaintMode(0);
        if (on && m_RigMode) SetRigMode(0);
        if (m_Modal.Kind != ModalKind::None) EndModal(false);
        m_Tool = Tool::None;
        m_Sculpting = on;
        m_SculptValid = false;
        m_Stroking = false;
        m_SculptAdjust = 0;
        m_Pressing = m_BoxSelecting = false;
        if (!on) ++m_GeometryVersion; // (Edit Mode works out what's visible again)
    }

    void MeshEditMode::EnsureSculptMesh()
    {
        if (m_SculptValid) return;
        m_Sculpt = sc::SculptMesh::FromEditable(m_Mesh);
        m_SculptValid = true;
    }

    void MeshEditMode::SculptPreview()
    {
        ++m_GeometryVersion;
        AssetManager::SetMeshData(m_Asset, m_Sculpt.ToMeshData(m_Mesh.MaterialSlots.size()));
    }

    void MeshEditMode::SculptCommit()
    {
        bool rebuilt = m_Sculpt.TopologyChanged();
        m_Sculpt.WriteTo(m_Mesh);
        if (rebuilt)
        {
            m_SculptValid = false; // (renumbered: built again from the model)
            Log::Info("Sculpt: {} vertices, {} faces", m_Mesh.VertexCount(), m_Mesh.FaceCount());
        }
        Commit();
    }

    void MeshEditMode::DoVoxelRemesh()
    {
        EnsureSculptMesh();
        Record("Voxel Remesh");
        m_Sculpt.WriteTo(m_Mesh);
        m_Mesh = sc::VoxelRemesh(m_Mesh, m_VoxelSize);
        m_Mesh.Changed();
        m_SculptValid = false;
        Commit();
        Log::Info("Voxel Remesh: {} vertices, {} faces", m_Mesh.VertexCount(), m_Mesh.FaceCount());
    }

    // ---- Rays and sizes

    void MeshEditMode::SculptRay(const ImVec2& mouse, glm::vec3& from, glm::vec3& dir) const
    {
        glm::mat4 inv = glm::inverse(m_ViewProj);
        glm::vec2 ndc((mouse.x - m_Origin.x) / m_Size.x * 2.0f - 1.0f, 1.0f - (mouse.y - m_Origin.y) / m_Size.y * 2.0f);
        glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        glm::vec3 wa = glm::vec3(a) / a.w, wb = glm::vec3(b) / b.w;
        glm::mat4 toModel = glm::inverse(m_World);
        from = glm::vec3(toModel * glm::vec4(wa, 1.0f));
        dir = glm::normalize(glm::vec3(toModel * glm::vec4(wb, 1.0f)) - from);
    }

    float MeshEditMode::ModelPerPixel(const glm::vec3& at) const
    {
        glm::vec3 world = glm::vec3(m_World * glm::vec4(at, 1.0f));
        glm::vec3 right = glm::normalize(glm::vec3(glm::inverse(m_View)[0]));
        auto screen = [&](const glm::vec3& p) {
            glm::vec4 c = m_ViewProj * glm::vec4(p, 1.0f);
            float w = std::max(c.w, 1e-6f);
            return glm::vec2(c.x / w * 0.5f * m_Size.x, c.y / w * 0.5f * m_Size.y);
        };
        float pixels = glm::length(screen(world + right) - screen(world));
        float scale = (glm::length(glm::vec3(m_World[0])) + glm::length(glm::vec3(m_World[1])) + glm::length(glm::vec3(m_World[2]))) / 3.0f;
        return pixels > 1e-6f ? 1.0f / pixels / std::max(scale, 1e-6f) : 0.01f;
    }

    sc::BrushSettings MeshEditMode::StrokeBrush(const glm::vec3& at, const glm::vec3& viewDir) const
    {
        sc::BrushSettings b;
        b.Type = m_StrokeType;
        b.Radius = m_BrushRadiusPx * ModelPerPixel(at);
        b.Strength = m_BrushStrength[(size_t)m_StrokeType];
        b.Invert = m_StrokeInvert;
        b.FrontFacesOnly = m_FrontFacesOnly && m_StrokeType != sc::Brush::Grab && m_StrokeType != sc::Brush::SnakeHook;
        b.ViewDir = viewDir;
        b.Symmetry = m_Symmetry;
        return b;
    }

    // ---- Strokes

    void MeshEditMode::UpdateStroke(bool hovered, bool blocked)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        bool overToolbar = mouse.x >= m_ToolbarMin.x && mouse.x <= m_ToolbarMax.x && mouse.y >= m_ToolbarMin.y && mouse.y <= m_ToolbarMax.y;
        if (!m_Stroking)
        {
            if (!(hovered && !blocked && !m_OverUI && !overToolbar && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return;
            glm::vec3 from, dir, n;
            SculptRay(mouse, from, dir);
            float t;
            if (!m_Sculpt.Raycast(from, dir, t, n)) return; // (strokes start on the surface)
            m_StrokeType = io.KeyShift ? sc::Brush::Smooth : m_BrushType;
            m_StrokeInvert = io.KeyCtrl != m_BrushInvert;
            m_StrokeStart = m_StrokeLast = from + dir * t;
            m_StrokeView = dir;
            m_StrokeMouse = mouse;
            m_StrokeMoved = false;
            Record((std::string("Sculpt: ") + sc::BrushName(m_StrokeType)).c_str());
            m_Stroking = true;
            sc::BrushSettings b = StrokeBrush(m_StrokeStart, dir);
            m_Sculpt.DetailSize = m_DynTopo ? (m_ConstantDetail ? m_DetailSize : m_DetailPx * ModelPerPixel(m_StrokeStart)) : 0.0f;
            m_Sculpt.BeginStroke(m_StrokeStart, b);
            if (m_StrokeType != sc::Brush::Grab && m_StrokeType != sc::Brush::SnakeHook)
            {
                m_Sculpt.Dab(m_StrokeStart, b);
                SculptPreview();
            }
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_Sculpt.EndStroke();
            m_Stroking = false;
            SculptCommit();
            return;
        }
        if (Dist(mouse, m_StrokeMouse) < 0.5f) return;
        glm::vec3 from, dir;
        bool changed = false;
        if (m_StrokeType == sc::Brush::Grab || m_StrokeType == sc::Brush::SnakeHook)
        {
            // The mouse carried onto a plane facing the view through where the stroke is.
            SculptRay(mouse, from, dir);
            glm::vec3 anchor = m_StrokeType == sc::Brush::Grab ? m_StrokeStart : m_StrokeLast, p;
            if (RayPlane(from, dir, anchor, m_StrokeView, p))
            {
                sc::BrushSettings b = StrokeBrush(anchor, m_StrokeView);
                if (m_StrokeType == sc::Brush::Grab) m_Sculpt.Dab(m_StrokeStart, b, p - m_StrokeStart);
                else
                {
                    m_Sculpt.Dab(m_StrokeLast, b, p - m_StrokeLast);
                    m_StrokeLast = p;
                }
                changed = true;
            }
            m_StrokeMouse = mouse;
        }
        else
        {
            // Dabs every so often along the mouse's path, each on the surface under it.
            float spacing = std::max(2.0f, m_BrushRadiusPx * 0.15f);
            float length = Dist(mouse, m_StrokeMouse);
            int steps = std::min((int)(length / spacing), 8);
            for (int i = 1; i <= steps; ++i)
            {
                float k = (float)i / (float)steps;
                ImVec2 at(m_StrokeMouse.x + (mouse.x - m_StrokeMouse.x) * k, m_StrokeMouse.y + (mouse.y - m_StrokeMouse.y) * k);
                SculptRay(at, from, dir);
                float t;
                glm::vec3 n;
                if (!m_Sculpt.Raycast(from, dir, t, n)) continue;
                glm::vec3 hit = from + dir * t;
                sc::BrushSettings b = StrokeBrush(hit, dir);
                if (m_DynTopo && !m_ConstantDetail) m_Sculpt.DetailSize = m_DetailPx * ModelPerPixel(hit);
                m_Sculpt.Dab(hit, b);
                m_StrokeLast = hit;
                changed = true;
            }
            if (steps > 0) m_StrokeMouse = mouse;
        }
        if (changed) SculptPreview();
    }

    void MeshEditMode::UpdateSculptAdjust()
    {
        // F / Shift+F: the distance from the circle's center sets the radius (or strength) until a click.
        ImVec2 mouse = ImGui::GetIO().MousePos;
        float d = Dist(mouse, m_AdjustCenter);
        float& strength = m_BrushStrength[(size_t)m_BrushType];
        if (m_SculptAdjust == 1) m_BrushRadiusPx = std::clamp(d, 2.0f, 2000.0f);
        else strength = std::clamp(d / 200.0f, 0.0f, 1.0f);
        bool later = ImGui::GetFrameCount() > m_AdjustFrame;
        if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            m_SculptAdjust = 0;
        else if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
        {
            if (m_SculptAdjust == 1) m_BrushRadiusPx = m_AdjustStart;
            else strength = m_AdjustStart;
            m_SculptAdjust = 0;
        }
    }

    // ---- Keys

    void MeshEditMode::HandleSculptKeys()
    {
        using Shortcuts::Pressed;
        for (int b = 0; b < (int)sc::Brush::Count; ++b)
            if (Pressed(kBrushKeys[b])) m_BrushType = (sc::Brush)b;
        if (Pressed("sculpt.radius")) m_SculptAdjust = 1;
        else if (Pressed("sculpt.strength")) m_SculptAdjust = 2;
        if (Pressed("mesh.sculptMode")) SetSculpting(false);
        if (Pressed("sculpt.smaller")) m_BrushRadiusPx = std::max(2.0f, m_BrushRadiusPx / 1.15f);
        if (Pressed("sculpt.larger")) m_BrushRadiusPx = std::min(2000.0f, m_BrushRadiusPx * 1.15f);
        if (Pressed("sculpt.dyntopo")) m_DynTopo = !m_DynTopo;
        if (Pressed("sculpt.remesh")) DoVoxelRemesh();
        if (Pressed("sculpt.clearMask")) SculptOperate("Clear Mask", [&] { m_Sculpt.ClearMask(); });
        if (Pressed("sculpt.invertMask")) SculptOperate("Invert Mask", [&] { m_Sculpt.InvertMask(); });
    }

    // ---- The view

    bool MeshEditMode::UpdateSculptView(bool hovered, bool focused, bool blocked)
    {
        EnsureSculptMesh();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        DrawSculptToolbar(m_Origin);
        ImGui::SetCursorScreenPos(cursor);
        m_OverUI = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (m_SculptAdjust) UpdateSculptAdjust();
        else
        {
            if ((hovered || focused) && !m_Stroking && !ImGui::GetIO().WantTextInput)
            {
                // F and Shift+F start from the circle around the mouse.
                int before = m_SculptAdjust;
                HandleSculptKeys();
                if (m_SculptAdjust && !before)
                {
                    ImVec2 mouse = ImGui::GetIO().MousePos;
                    m_AdjustStart = m_SculptAdjust == 1 ? m_BrushRadiusPx : m_BrushStrength[(size_t)m_BrushType];
                    float d = m_SculptAdjust == 1 ? m_BrushRadiusPx : m_AdjustStart * 200.0f;
                    m_AdjustCenter = ImVec2(mouse.x - d, mouse.y);
                    m_AdjustFrame = ImGui::GetFrameCount();
                }
            }
            if (m_Sculpting) UpdateStroke(hovered, blocked); // (a key may have left Sculpt Mode)
        }
        // The mask: masked triangles facing the camera darkened.
        if (std::any_of(m_Sculpt.Mask.begin(), m_Sculpt.Mask.end(), [](float v) { return v > 0.0f; }))
        {
            glm::mat4 mvp = m_ViewProj * m_World;
            auto project = [&](const glm::vec3& p, ImVec2& out) {
                glm::vec4 c = mvp * glm::vec4(p, 1.0f);
                if (c.w <= 1e-6f) return false;
                out = ImVec2(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
                return true;
            };
            // Shaded per vertex and without edge antialiasing (which would outline every triangle).
            ImDrawListFlags saved = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            ImVec2 white = ImGui::GetFontTexUvWhitePixel();
            auto shade = [](float m) { return IM_COL32(0, 0, 0, (int)(std::clamp(m, 0.0f, 1.0f) * 150.0f)); };
            for (size_t t = 0; t < m_Sculpt.Tris.size(); ++t)
            {
                if (m_Sculpt.IsDead((uint32_t)t)) continue;
                const glm::uvec3& tri = m_Sculpt.Tris[t];
                float ma = m_Sculpt.Mask[tri.x], mb = m_Sculpt.Mask[tri.y], mc = m_Sculpt.Mask[tri.z];
                if (ma + mb + mc <= 0.01f) continue;
                const glm::vec3 &a = m_Sculpt.Positions[tri.x], &b = m_Sculpt.Positions[tri.y], &c = m_Sculpt.Positions[tri.z];
                if (glm::dot(glm::cross(b - a, c - a), m_CameraLocal - a) <= 0.0f) continue;
                ImVec2 pa, pb, pc;
                if (!project(a, pa) || !project(b, pb) || !project(c, pc)) continue;
                dl->PrimReserve(3, 3);
                dl->PrimVtx(pa, white, shade(ma));
                dl->PrimVtx(pb, white, shade(mb));
                dl->PrimVtx(pc, white, shade(mc));
            }
            dl->Flags = saved;
        }
        // The brush circle.
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool invert = (ImGui::GetIO().KeyCtrl != m_BrushInvert);
        ImU32 col = m_BrushType == sc::Brush::Mask ? IM_COL32(120, 120, 255, 220) : invert ? IM_COL32(120, 180, 255, 220) : IM_COL32(255, 255, 255, 220);
        if (m_SculptAdjust)
        {
            dl->AddCircle(m_AdjustCenter, m_SculptAdjust == 1 ? m_BrushRadiusPx : m_BrushStrength[(size_t)m_BrushType] * 200.0f, col, 64, 2.0f);
            dl->AddCircle(m_AdjustCenter, m_BrushRadiusPx, IM_COL32(255, 255, 255, 90), 64, 1.0f);
        }
        else if ((hovered || m_Stroking) && !m_OverUI)
            dl->AddCircle(mouse, m_BrushRadiusPx, col, 64, 1.5f);

        // Status.
        char buf[300];
        std::snprintf(buf, sizeof(buf), "Sculpt: %s   radius %.0f px (%s)   strength %.2f (%s)%s   %zu vertices, %zu triangles   Ctrl: invert, Shift: smooth",
                      sc::BrushName(m_BrushType), m_BrushRadiusPx, Shortcuts::Label("sculpt.radius").c_str(), m_BrushStrength[(size_t)m_BrushType],
                      Shortcuts::Label("sculpt.strength").c_str(), m_DynTopo ? "   dynamic topology" : "", m_Sculpt.LiveVertexCount(),
                      m_Sculpt.LiveTriangleCount());
        dl->AddText(ImVec2(m_Origin.x + 8.0f, m_Origin.y + m_Size.y - 24.0f), IM_COL32(255, 255, 255, 230), buf);
        return m_Stroking || m_SculptAdjust != 0;
    }

    void MeshEditMode::DrawSculptToolbar(const ImVec2& origin)
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 28.0f));
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
        auto toggle = [](const char* label, bool on, const std::string& tip) {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool pressed = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            if (!tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip.c_str());
            return pressed;
        };
        if (toggle("Edit", false, "Back to Edit Mode (" + Shortcuts::Label("mesh.sculptMode") + ")")) SetSculpting(false);
        ImGui::SameLine(0.0f, 2.0f);
        toggle("Sculpt", true, "");
        ImGui::SameLine();
        for (int b = 0; b < (int)sc::Brush::Count; ++b)
        {
            if (b) ImGui::SameLine(0.0f, 2.0f);
            std::string tip = std::string(kBrushTips[b]);
            std::string key = Shortcuts::Label(kBrushKeys[b]);
            if (!key.empty()) tip += " (" + key + ")";
            if (toggle(sc::BrushName((sc::Brush)b), m_BrushType == (sc::Brush)b, tip)) m_BrushType = (sc::Brush)b;
        }
        ImGui::SameLine();
        if (ImGui::Button("Done")) Exit();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave Edit Mode (%s)", Shortcuts::Label("mesh.editMode").c_str());

        // Second row: the brush and the mesh.
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("##radius", &m_BrushRadiusPx, 0.5f, 2.0f, 2000.0f, "%.0f px");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius on screen (%s; [ and ] too)", Shortcuts::Label("sculpt.radius").c_str());
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("##strength", &m_BrushStrength[(size_t)m_BrushType], 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strength of this brush (%s)", Shortcuts::Label("sculpt.strength").c_str());
        ImGui::SameLine();
        if (toggle(m_BrushInvert ? "Subtract" : "Add", m_BrushInvert, "Which way the brush works (Ctrl flips it while held)")) m_BrushInvert = !m_BrushInvert;
        ImGui::SameLine();
        if (toggle("Front Faces", m_FrontFacesOnly, "Only parts facing the camera (else the brush reaches through to the far side)"))
            m_FrontFacesOnly = !m_FrontFacesOnly;
        ImGui::SameLine();
        ImGui::TextUnformatted("Mirror");
        for (int a = 0; a < 3; ++a)
        {
            ImGui::SameLine(0.0f, 2.0f);
            char label[8];
            std::snprintf(label, sizeof(label), "%c##sym", "XYZ"[a]);
            if (toggle(label, (m_Symmetry >> a) & 1, std::string("Strokes also work mirrored across the model's ") + "XYZ"[a] + " plane"))
                m_Symmetry ^= (uint8_t)(1 << a);
        }
        ImGui::SameLine();
        if (toggle("Dyntopo", m_DynTopo,
                   "Dynamic topology (" + Shortcuts::Label("sculpt.dyntopo") +
                       "): the brush adds and removes triangles to keep the detail size. The model becomes triangles and loses its UVs."))
            m_DynTopo = !m_DynTopo;
        if (m_DynTopo)
        {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(80.0f);
            if (m_ConstantDetail) ImGui::DragFloat("##detail", &m_DetailSize, 0.001f, 0.001f, 10.0f, "%.3f m");
            else ImGui::DragFloat("##detail", &m_DetailPx, 0.1f, 2.0f, 100.0f, "%.0f px");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Detail: the longest edge under the brush (on screen, or in the model's units)");
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(84.0f);
            const char* detailModes[] = { "Relative", "Constant" };
            int dm = m_ConstantDetail ? 1 : 0;
            if (ImGui::Combo("##detailmode", &dm, detailModes, 2)) m_ConstantDetail = dm == 1;
            if (m_ConstantDetail)
            {
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::Button("Flood Fill"))
                    SculptOperate("Detail Flood Fill", [&] { m_Sculpt.FloodFillDetail(m_DetailSize); });
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The whole model to the detail size");
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("##voxel", &m_VoxelSize, 0.001f, 0.001f, 10.0f, "%.3f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Voxel size for Remesh");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Remesh")) DoVoxelRemesh();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Voxel Remesh (%s): a new, even all-quad surface over the model's volume (overlapping parts join; UVs are lost)",
                              Shortcuts::Label("sculpt.remesh").c_str());
        ImGui::SameLine();
        if (ImGui::Button("Mask")) ImGui::OpenPopup("SculptMaskMenu");
        if (ImGui::BeginPopup("SculptMaskMenu"))
        {
            if (ImGui::MenuItem("Clear", Shortcuts::Label("sculpt.clearMask").c_str())) SculptOperate("Clear Mask", [&] { m_Sculpt.ClearMask(); });
            if (ImGui::MenuItem("Invert", Shortcuts::Label("sculpt.invertMask").c_str())) SculptOperate("Invert Mask", [&] { m_Sculpt.InvertMask(); });
            if (ImGui::MenuItem("Smooth")) SculptOperate("Smooth Mask", [&] { m_Sculpt.SmoothMask(2); });
            if (ImGui::MenuItem("Fill")) SculptOperate("Fill Mask", [&] { std::fill(m_Sculpt.Mask.begin(), m_Sculpt.Mask.end(), 1.0f); });
            ImGui::Separator();
            if (ImGui::MenuItem("Smooth Unmasked")) SculptOperate("Smooth", [&] { m_Sculpt.SmoothAll(3); });
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        m_ToolbarMin = ImGui::GetItemRectMin();
        m_ToolbarMax = ImGui::GetItemRectMax();
    }
}
