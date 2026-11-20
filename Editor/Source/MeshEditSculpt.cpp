#include "MeshEditMode.h"
#include "EditorUI.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Modeling/FaceSets.h>
#include <Dimension/Scene/Components.h>
#include <algorithm>
#include <chrono>
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
                                     "sculpt.pinch", "sculpt.crease", "sculpt.grab", "sculpt.snakeHook", "sculpt.mask",
                                     "sculpt.pose", "sculpt.boundary", "sculpt.cloth", "sculpt.faceSet" };
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
                                     "Paints the mask: masked parts don't move (Ctrl erases)",
                                     "Bends the part under the brush about a joint at the brush's edge (put the brush over a limb's end and drag)",
                                     "Drags the open edges under the brush, the surface behind them following",
                                     "Drags the surface like cloth: it keeps its size, so it folds and wrinkles where it bunches up",
                                     "Draw Face Sets: paints a new face set each stroke (Ctrl: grows the one under the stroke's start)" };
    }

    void MeshEditMode::SetSculpting(bool on)
    {
        if (!m_Active || on == m_Sculpting) return;
        if (on && m_PaintMode) SetPaintMode(0);
        if (on && m_RigMode) SetRigMode(0);
        if (on && m_CurveMode) SetCurveMode(false);
        if (m_Modal.Kind != ModalKind::None) EndModal(false);
        m_Tool = Tool::None;
        m_Sculpting = on;
        m_SculptValid = false;
        if (FaceSets::AnyHidden(m_Mesh)) SyncRenderMesh(); // (Sculpt Mode leaves hidden face sets out of the view)
        m_Stroking = false;
        m_SculptAdjust = 0;
        m_Pressing = m_BoxSelecting = false;
        if (!on) ++m_GeometryVersion; // (Edit Mode works out what's visible again)
    }

    int MeshEditMode::MultiresIndex() const
    {
        for (size_t i = 0; i < m_Mesh.Modifiers.size(); ++i)
        {
            const Modifier& mod = m_Mesh.Modifiers[i];
            if (mod.Enabled && mod.Type == ModifierType::Multires && mod.Levels > 0) return (int)i;
        }
        return -1;
    }

    void MeshEditMode::EnsureSculptMesh()
    {
        if (m_SculptValid) return;
        // With a Multires modifier the strokes go into its finest level (the model itself stays the base cage).
        m_SculptMultires = MultiresIndex();
        if (m_SculptMultires >= 0)
        {
            const Modifier& mod = m_Mesh.Modifiers[(size_t)m_SculptMultires];
            m_Sculpt = sc::SculptMesh::FromEditable(ModifierOps::MultiresMesh(m_Mesh, mod, mod.Levels));
        }
        else m_Sculpt = sc::SculptMesh::FromEditable(m_Mesh);
        m_SculptValid = true;
    }

    void MeshEditMode::SculptPreview()
    {
        ++m_GeometryVersion;
        if (m_SculptMultires < 0 && m_LiveModifiers && m_Mesh.HasActiveModifiers() && m_LiveModifierMs < 40.0f && !FaceSets::AnyHidden(m_Mesh))
        {
            // The model as it would be written back, with its modifiers (dropped for the rest of the stroke when it
            // gets slow).
            auto start = std::chrono::steady_clock::now();
            EditableMesh view = m_Mesh;
            m_Sculpt.WriteTo(view);
            AssetManager::SetMeshData(m_Asset, view.ToMeshData());
            m_LiveModifierMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            return;
        }
        AssetManager::SetMeshData(m_Asset, m_Sculpt.ToMeshData(m_Mesh.MaterialSlots.size()));
    }

    void MeshEditMode::SculptCommit()
    {
        m_LiveModifierMs = 0.0f;
        if (m_SculptMultires >= 0 && m_SculptMultires < (int)m_Mesh.Modifiers.size() &&
            m_Mesh.Modifiers[(size_t)m_SculptMultires].Type == ModifierType::Multires)
        {
            ModifierOps::MultiresStore(m_Mesh, m_Mesh.Modifiers[(size_t)m_SculptMultires], m_Sculpt.Positions);
            Commit();
            return;
        }
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
        if (m_SculptMultires >= 0)
        {
            Log::Warn("Voxel Remesh: apply or remove the Multiresolution modifier first");
            return;
        }
        Record("Voxel Remesh");
        m_Sculpt.WriteTo(m_Mesh);
        m_Mesh = sc::VoxelRemesh(m_Mesh, m_VoxelSize, 2, m_RemeshSharp);
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
        b.FrontFacesOnly = m_FrontFacesOnly && !sc::IsDragBrush(m_StrokeType);
        b.Texture = m_BrushTexAsset.IsValid() && !m_BrushTex.Values.empty() ? &m_BrushTex : nullptr;
        b.ViewDir = viewDir;
        b.Symmetry = m_Symmetry;
        b.FaceSet = m_StrokeFaceSet;
        b.AutoMaskFaceSet = m_AutoMaskFaceSet;
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
            if (m_StrokeType == sc::Brush::FaceSet)
            {
                if (m_SculptMultires >= 0)
                {
                    Log::Warn("Face Sets brush: not on a Multiresolution level (make face sets from the menu, or in Edit Mode)");
                    return;
                }
                // A new set each stroke; inverted, the one under the stroke's start grows.
                int32_t under = m_Sculpt.FaceSetAt(from, dir);
                m_StrokeFaceSet = m_StrokeInvert && under >= 0 ? under : m_Sculpt.NextFaceSet();
            }
            m_StrokeStart = m_StrokeLast = from + dir * t;
            m_StrokeView = dir;
            m_StrokeMouse = mouse;
            m_StrokeMoved = false;
            Record((std::string("Sculpt: ") + sc::BrushName(m_StrokeType)).c_str());
            m_Stroking = true;
            sc::BrushSettings b = StrokeBrush(m_StrokeStart, dir);
            m_Sculpt.DetailSize = DynTopoOn() ? (m_ConstantDetail ? m_DetailSize : m_DetailPx * ModelPerPixel(m_StrokeStart)) : 0.0f;
            m_Sculpt.BeginStroke(m_StrokeStart, b);
            if (!sc::IsDragBrush(m_StrokeType))
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
        if (sc::IsDragBrush(m_StrokeType))
        {
            // The mouse carried onto a plane facing the view through where the stroke is.
            SculptRay(mouse, from, dir);
            bool anchored = sc::IsAnchoredBrush(m_StrokeType);
            glm::vec3 anchor = anchored ? m_StrokeStart : m_StrokeLast, p;
            if (RayPlane(from, dir, anchor, m_StrokeView, p))
            {
                sc::BrushSettings b = StrokeBrush(anchor, m_StrokeView);
                if (anchored) m_Sculpt.Dab(m_StrokeStart, b, p - m_StrokeStart);
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
                if (DynTopoOn() && !m_ConstantDetail) m_Sculpt.DetailSize = m_DetailPx * ModelPerPixel(hit);
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
        int32_t set = m_HoverFaceSet;
        if (Pressed("sculpt.faceSetIsolate") && set >= 0) FaceSetOperate("Isolate Face Set", [&] { FaceSets::ToggleIsolate(m_Mesh, set); });
        if (Pressed("sculpt.faceSetHide") && set >= 0) FaceSetOperate("Hide Face Set", [&] { FaceSets::Hide(m_Mesh, set); });
        if (Pressed("sculpt.revealAll") && FaceSets::AnyHidden(m_Mesh)) FaceSetOperate("Reveal Face Sets", [&] { FaceSets::RevealAll(m_Mesh); });
    }

    // ---- Face sets

    void MeshEditMode::FaceSetOperate(const char* name, const std::function<void()>& fn)
    {
        // On the model itself (the base mesh with a Multires modifier: its levels inherit the sets), then the sculpt
        // mesh built again from it.
        EnsureSculptMesh();
        Record(name);
        if (m_SculptMultires < 0) m_Sculpt.WriteTo(m_Mesh);
        fn();
        m_Mesh.Changed();
        m_SculptValid = false;
        Commit();
        EnsureSculptMesh();
        SculptPreview();
    }

    void MeshEditMode::DrawFaceSetMenu()
    {
        int32_t set = m_MenuFaceSet;
        char label[96];
        if (set >= 0) std::snprintf(label, sizeof(label), "Face set %d (under the mouse)", set);
        else std::snprintf(label, sizeof(label), "No face set under the mouse");
        ImGui::TextDisabled("%s", label);
        if (ImGui::MenuItem("From Masked")) FaceSetOperate("Face Set from Masked", [&] { FaceSets::FromMasked(m_Mesh); });
        if (ImGui::MenuItem("From Visible")) FaceSetOperate("Face Set from Visible", [&] { FaceSets::FromVisible(m_Mesh); });
        if (ImGui::MenuItem("From Edit Mode Selection")) FaceSetOperate("Face Set from Selection", [&] { FaceSets::FromSelection(m_Mesh); });
        if (ImGui::BeginMenu("Init Face Sets"))
        {
            const char* modes[] = { "By Loose Parts", "By Materials", "By Normals", "By UV Seams", "By Sharp Edges" };
            for (int i = 0; i < 5; ++i)
                if (ImGui::MenuItem(modes[i]))
                    FaceSetOperate("Init Face Sets", [&] {
                        int n = FaceSets::InitFrom(m_Mesh, (FaceSets::InitMode)i, m_FaceSetAngle);
                        Log::Info("Init Face Sets: {} sets", n);
                    });
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Angle", &m_FaceSetAngle, 0.5f, 1.0f, 180.0f, "%.0f deg");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("By Normals: faces meeting at a sharper bend than this start a new set");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Grow", nullptr, false, set >= 0)) FaceSetOperate("Grow Face Set", [&] { FaceSets::Grow(m_Mesh, set); });
        if (ImGui::MenuItem("Shrink", nullptr, false, set >= 0)) FaceSetOperate("Shrink Face Set", [&] { FaceSets::Shrink(m_Mesh, set); });
        ImGui::Separator();
        if (ImGui::MenuItem("Isolate", Shortcuts::Label("sculpt.faceSetIsolate").c_str(), false, set >= 0))
            FaceSetOperate("Isolate Face Set", [&] { FaceSets::ToggleIsolate(m_Mesh, set); });
        if (ImGui::MenuItem("Hide", Shortcuts::Label("sculpt.faceSetHide").c_str(), false, set >= 0))
            FaceSetOperate("Hide Face Set", [&] { FaceSets::Hide(m_Mesh, set); });
        if (ImGui::MenuItem("Reveal All", Shortcuts::Label("sculpt.revealAll").c_str(), false, FaceSets::AnyHidden(m_Mesh)))
            FaceSetOperate("Reveal Face Sets", [&] { FaceSets::RevealAll(m_Mesh); });
        ImGui::Separator();
        if (ImGui::MenuItem("Mask Others", nullptr, false, set >= 0)) FaceSetOperate("Mask from Face Set", [&] { FaceSets::MaskFromSet(m_Mesh, set); });
        if (ImGui::MenuItem("Mask This Set", nullptr, false, set >= 0))
            FaceSetOperate("Mask from Face Set", [&] { FaceSets::MaskFromSet(m_Mesh, set, true); });
        ImGui::Separator();
        if (ImGui::MenuItem("Show Colors", nullptr, m_ShowFaceSets)) m_ShowFaceSets = !m_ShowFaceSets;
        if (ImGui::MenuItem("Randomize Colors")) ++m_FaceSetSeed;
        if (ImGui::MenuItem("Clear All")) FaceSetOperate("Clear Face Sets", [&] {
            for (Face& f : m_Mesh.Faces) f.FaceSet = 0;
        });
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
        if (!m_Sculpting) return false;
        EnsureSculptMesh(); // (a face set operation may have rebuilt the model)
        if (hovered && !m_Stroking && !m_OverUI)
        {
            glm::vec3 from, dir;
            SculptRay(ImGui::GetIO().MousePos, from, dir);
            m_HoverFaceSet = m_Sculpt.FaceSetAt(from, dir);
        }
        // Face sets: each triangle facing the camera tinted with its set's color.
        if (m_ShowFaceSets && m_Sculpt.HasFaceSets())
        {
            glm::mat4 mvp = m_ViewProj * m_World;
            auto project = [&](const glm::vec3& p, ImVec2& out) {
                glm::vec4 c = mvp * glm::vec4(p, 1.0f);
                if (c.w <= 1e-6f) return false;
                out = ImVec2(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
                return true;
            };
            ImDrawListFlags saved = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            ImVec2 white = ImGui::GetFontTexUvWhitePixel();
            int32_t lastSet = INT32_MIN;
            ImU32 col = 0;
            for (size_t t = 0; t < m_Sculpt.Tris.size(); ++t)
            {
                if (m_Sculpt.IsDead((uint32_t)t) || m_Sculpt.TriHidden[t] || m_Sculpt.TriFaceSet[t] == 0) continue;
                const glm::uvec3& tri = m_Sculpt.Tris[t];
                const glm::vec3 &a = m_Sculpt.Positions[tri.x], &b = m_Sculpt.Positions[tri.y], &c = m_Sculpt.Positions[tri.z];
                if (glm::dot(glm::cross(b - a, c - a), m_CameraLocal - a) <= 0.0f) continue;
                ImVec2 pa, pb, pc;
                if (!project(a, pa) || !project(b, pb) || !project(c, pc)) continue;
                if (m_Sculpt.TriFaceSet[t] != lastSet)
                {
                    lastSet = m_Sculpt.TriFaceSet[t];
                    glm::vec3 k = FaceSets::Color(lastSet, m_FaceSetSeed);
                    bool hot = lastSet == m_HoverFaceSet && hovered && !m_Stroking;
                    col = IM_COL32((int)(k.r * 255.0f), (int)(k.g * 255.0f), (int)(k.b * 255.0f), hot ? 130 : 95);
                }
                dl->PrimReserve(3, 3);
                dl->PrimVtx(pa, white, col);
                dl->PrimVtx(pb, white, col);
                dl->PrimVtx(pc, white, col);
            }
            dl->Flags = saved;
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
                if (m_Sculpt.IsDead((uint32_t)t) || m_Sculpt.TriHidden[t]) continue;
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
        ImU32 col = m_BrushType == sc::Brush::Mask ? IM_COL32(120, 120, 255, 220)
                    : m_BrushType == sc::Brush::FaceSet ? IM_COL32(255, 200, 90, 220)
                    : invert ? IM_COL32(120, 180, 255, 220) : IM_COL32(255, 255, 255, 220);
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

    void MeshEditMode::LoadBrushTexture()
    {
        std::vector<uint32_t> pixels;
        glm::uvec2 size(0);
        if (m_BrushTexAsset.IsValid() && AssetManager::ReadTexturePixels(m_BrushTexAsset, pixels, size) && size.x && size.y)
        {
            sc::BrushTexture loaded = sc::BrushTexture::FromRGBA8(pixels.data(), size.x, size.y);
            m_BrushTex.Values = std::move(loaded.Values);
            m_BrushTex.Width = loaded.Width;
            m_BrushTex.Height = loaded.Height;
            return;
        }
        if (m_BrushTexAsset.IsValid()) Log::Warn("Sculpt: couldn't read the brush texture's pixels");
        m_BrushTexAsset = AssetHandle::None();
        m_BrushTex.Values.clear();
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
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Texture", m_BrushTexAsset.IsValid(), "A texture varying the brushes' strength (white full, black none)")) ImGui::OpenPopup("SculptTexture");
        if (ImGui::BeginPopup("SculptTexture"))
        {
            ImGui::SetNextItemWidth(220.0f);
            if (UI::AssetField("Texture", m_BrushTexAsset, AssetType::Texture).Changed) LoadBrushTexture();
            const char* mappings[] = { "View (one per dab)", "Tiled (over the view)" };
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo("Mapping", &m_BrushTex.Mapping, mappings, 2);
            if (m_BrushTex.Mapping == 1)
            {
                ImGui::SetNextItemWidth(160.0f);
                ImGui::DragFloat("Size", &m_BrushTex.Scale, 0.005f, 0.001f, 100.0f, "%.3f m");
            }
            ImGui::SetNextItemWidth(160.0f);
            ImGui::DragFloat("Angle", &m_BrushTex.Angle, 1.0f, -360.0f, 360.0f, "%.0f deg");
            if (ImGui::Button("None"))
            {
                m_BrushTexAsset = AssetHandle::None();
                m_BrushTex.Values.clear();
            }
            ImGui::EndPopup();
        }
        if (m_Mesh.HasActiveModifiers())
        {
            ImGui::SameLine(0.0f, 2.0f);
            if (toggle("Modifiers", m_LiveModifiers, "Strokes show the model's modifiers as they go (else just the base mesh until the stroke ends)"))
                m_LiveModifiers = !m_LiveModifiers;
        }
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
        if (m_SculptMultires >= 0)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Multires");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sculpting the Multiresolution modifier's finest level (its Subdivide adds detail; Dyntopo and Remesh are off)");
        }
        else if (toggle("Dyntopo", m_DynTopo,
                   "Dynamic topology (" + Shortcuts::Label("sculpt.dyntopo") +
                       "): the brush adds and removes triangles to keep the detail size. The model becomes triangles and loses its UVs."))
            m_DynTopo = !m_DynTopo;
        if (DynTopoOn())
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
        if (toggle("Sharp", m_RemeshSharp, "Remesh keeps corners and creases (else rounds them off a little)")) m_RemeshSharp = !m_RemeshSharp;
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Remesh")) DoVoxelRemesh();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Voxel Remesh (%s): a new, even all-quad surface over the model's volume (overlapping parts join; UVs are lost)",
                              Shortcuts::Label("sculpt.remesh").c_str());
        ImGui::SameLine();
        if (ImGui::Button("Face Sets"))
        {
            m_MenuFaceSet = m_HoverFaceSet;
            ImGui::OpenPopup("SculptFaceSetMenu");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Group faces to show, hide (%s isolates the set under the mouse, %s hides it, %s shows all) and mask by",
                              Shortcuts::Label("sculpt.faceSetIsolate").c_str(), Shortcuts::Label("sculpt.faceSetHide").c_str(),
                              Shortcuts::Label("sculpt.revealAll").c_str());
        if (ImGui::BeginPopup("SculptFaceSetMenu"))
        {
            DrawFaceSetMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Set Mask", m_AutoMaskFaceSet, "Auto-masking: a stroke only works on the face set it starts on"))
            m_AutoMaskFaceSet = !m_AutoMaskFaceSet;
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
