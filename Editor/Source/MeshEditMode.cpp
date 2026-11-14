#include "MeshEditMode.h"
#include "EditorActions.h"
#include "EditorPreferences.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Modeling/ModelExport.h>
#include <Dimension/Platform/FileDialogs.h>
#include <Dimension/Scene/Components.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Editor
{
    using namespace Dimension::Modeling;
    namespace ops = Dimension::Modeling::Ops;

    namespace
    {
        constexpr ImU32 kSelected = IM_COL32(255, 160, 40, 255);
        constexpr ImU32 kWire = IM_COL32(16, 16, 16, 230);
        constexpr ImU32 kWireXRay = IM_COL32(16, 16, 16, 140);
        constexpr ImU32 kSeam = IM_COL32(230, 60, 40, 255);
        constexpr ImU32 kSharp = IM_COL32(60, 200, 230, 255);
        constexpr ImU32 kFaceFill = IM_COL32(255, 160, 40, 60);
        constexpr ImU32 kVertex = IM_COL32(8, 8, 8, 255);
        constexpr float kOffscreen = -1e9f;

        float Length(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
        ImVec2 Sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }

        float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b)
        {
            ImVec2 ab = Sub(b, a), ap = Sub(p, a);
            float len2 = ab.x * ab.x + ab.y * ab.y;
            float t = len2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
            return Length(ImVec2(p.x - (a.x + ab.x * t), p.y - (a.y + ab.y * t)));
        }

        bool OnScreen(ImVec2 p) { return p.x > kOffscreen * 0.5f; }

        // A world-space ray through a point of the view.
        void RayAt(const glm::mat4& viewProj, const ImVec2& origin, const ImVec2& size, ImVec2 point, glm::vec3& from, glm::vec3& dir)
        {
            glm::mat4 inv = glm::inverse(viewProj);
            glm::vec2 ndc((point.x - origin.x) / size.x * 2.0f - 1.0f, 1.0f - (point.y - origin.y) / size.y * 2.0f);
            glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
            from = glm::vec3(a) / a.w;
            dir = glm::normalize(glm::vec3(b) / b.w - from);
        }

        ImVec2 Project(const glm::mat4& viewProj, const ImVec2& origin, const ImVec2& size, const glm::vec3& world)
        {
            glm::vec4 c = viewProj * glm::vec4(world, 1.0f);
            if (c.w <= 1e-6f) return ImVec2(kOffscreen, kOffscreen);
            glm::vec3 ndc = glm::vec3(c) / c.w;
            return ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (1.0f - ndc.y) * 0.5f * size.y);
        }

        bool RayPlane(const glm::vec3& from, const glm::vec3& dir, const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
        {
            float d = glm::dot(dir, normal);
            if (std::abs(d) < 1e-8f) return false;
            float t = glm::dot(point - from, normal) / d;
            hit = from + dir * t;
            return true;
        }

        // The parameter along the line point + axis * s closest to the ray.
        float ClosestOnAxis(const glm::vec3& from, const glm::vec3& dir, const glm::vec3& point, const glm::vec3& axis)
        {
            glm::vec3 w = point - from;
            float a = glm::dot(axis, axis), b = glm::dot(axis, dir), c = glm::dot(dir, dir), d = glm::dot(axis, w), e = glm::dot(dir, w);
            float den = a * c - b * b;
            if (std::abs(den) < 1e-10f) return 0.0f;
            return (b * e - c * d) / den;
        }

        float Snap(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

        fs::path UniqueFile(const fs::path& folder, const std::string& name, const char* ext)
        {
            fs::path p = folder / (name + ext);
            for (int i = 1; fs::exists(p); ++i) p = folder / (name + " " + std::to_string(i) + ext);
            return p;
        }

        const char* ModeName(SelectMode m) { return m == SelectMode::Vertex ? "Vertex" : m == SelectMode::Edge ? "Edge" : "Face"; }
    }

    // ---- Primitives and model assets

    const char* MeshEditMode::PrimitiveName(Primitive p)
    {
        static const char* names[] = { "Cube", "Plane", "Circle", "Grid", "UV Sphere", "Ico Sphere", "Cylinder", "Cone", "Torus" };
        return names[std::clamp((int)p, 0, (int)Primitive::Count - 1)];
    }

    EditableMesh MeshEditMode::MakePrimitive(Primitive p)
    {
        switch (p)
        {
        case Primitive::Cube: return Primitives::Cube(1.0f);
        case Primitive::Plane: return Primitives::Plane(1.0f);
        case Primitive::Circle: return Primitives::Circle(32, 0.5f);
        case Primitive::Grid: return Primitives::Grid(10, 10, 1.0f);
        case Primitive::UVSphere: return Primitives::UVSphere(32, 16, 0.5f);
        case Primitive::IcoSphere: return Primitives::IcoSphere(2, 0.5f);
        case Primitive::Cylinder: return Primitives::Cylinder(32, 0.5f, 1.0f);
        case Primitive::Cone: return Primitives::Cone(32, 0.5f, 0.0f, 1.0f);
        case Primitive::Torus: return Primitives::Torus(48, 12, 0.4f, 0.1f);
        default: return Primitives::Cube(1.0f);
        }
    }

    AssetHandle MeshEditMode::CreateModelAsset(const EditableMesh& mesh, const fs::path& folder, const std::string& name)
    {
        std::error_code ec;
        fs::create_directories(folder, ec);
        fs::path path = UniqueFile(folder, name, ".dmodel");
        EditableMesh copy = mesh;
        copy.Name = path.stem().string();
        if (!copy.Save(path))
        {
            Log::Error("Could not write the model {}", path.string());
            return AssetHandle::None();
        }
        return AssetManager::Import(path);
    }

    Entity MeshEditMode::CreateModelObject(EditorContext& ctx, Primitive p, Entity parent)
    {
        std::string name = PrimitiveName(p);
        std::erase(name, ' ');
        AssetHandle model = CreateModelAsset(MakePrimitive(p), AssetManager::GetAssetsDirectory() / "Models", name);
        if (!model.IsValid()) return Entity::Null();
        Entity e = CreateObject(ctx, ObjectKind::Empty, parent); // (records undo, selects it)
        auto& reg = ctx.ActiveScene().GetRegistry();
        reg.Get<TagComponent>(e).Name = PrimitiveName(p);
        reg.Emplace<MeshRendererComponent>(e).Mesh = model;
        ctx.MarkDirty();
        return e;
    }

    // ---- Entering and leaving

    void MeshEditMode::Toggle()
    {
        if (m_Active)
        {
            Exit();
            return;
        }
        if (m_Ctx.HasValidSelection()) Enter(m_Ctx.Selected);
    }

    bool MeshEditMode::Enter(Entity e)
    {
        if (m_Ctx.IsPlaying()) return false;
        Scene& scene = m_Ctx.ActiveScene();
        if (!scene.IsValid(e) || !scene.GetRegistry().Has<MeshRendererComponent>(e))
        {
            Log::Warn("Edit Mode needs an object with a Mesh Renderer");
            return false;
        }
        AssetHandle mesh = scene.GetRegistry().Get<MeshRendererComponent>(e).Mesh;
        const AssetMetadata* meta = AssetManager::GetMetadata(mesh);
        if (!mesh.IsValid())
        {
            Log::Warn("This Mesh Renderer has no mesh to edit");
            return false;
        }
        if (!meta || meta->Path.extension() != ".dmodel")
        {
            m_ConvertEntity = e; // ask first: an editable copy
            m_OpenConvert = true;
            return false;
        }
        fs::path file = AssetManager::GetAbsolutePath(mesh);
        EditableMesh loaded;
        if (!EditableMesh::Load(file, loaded)) return false;
        m_Mesh = std::move(loaded);
        m_Active = true;
        m_Entity = e;
        m_Asset = mesh;
        m_File = file;
        m_Dirty = false;
        m_Undo.clear();
        m_Redo.clear();
        m_Modal = {};
        m_Pressing = m_BoxSelecting = false;
        ops::FlushSelection(m_Mesh, m_Mode);
        ++m_GeometryVersion;
        return true;
    }

    void MeshEditMode::Exit()
    {
        if (!m_Active) return;
        if (IsModal()) EndModal(false);
        if (m_Dirty) Save();
        m_Active = false;
        m_Entity = Entity::Null();
        m_Asset = AssetHandle::None();
        m_Mesh = EditableMesh();
        m_Undo.clear();
        m_Redo.clear();
        m_Pressing = m_BoxSelecting = false;
    }

    void MeshEditMode::Save()
    {
        if (!m_Active) return;
        m_Mesh.Name = m_File.stem().string();
        if (m_Mesh.Save(m_File)) m_Dirty = false;
        else Log::Error("Could not write the model {}", m_File.string());
    }

    void MeshEditMode::Update()
    {
        if (!m_Active) return;
        Scene& scene = m_Ctx.ActiveScene();
        bool valid = !m_Ctx.IsPlaying() && scene.IsValid(m_Entity) && m_Ctx.Selected == m_Entity &&
                     scene.GetRegistry().Has<MeshRendererComponent>(m_Entity) && scene.GetRegistry().Get<MeshRendererComponent>(m_Entity).Mesh == m_Asset;
        if (!valid)
        {
            Exit();
            return;
        }
        // Write the file once a change is done (nothing held).
        if (m_Dirty && !IsModal() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) Save();
    }

    void MeshEditMode::SetSelectMode(SelectMode mode)
    {
        if (mode == m_Mode) return;
        Record((std::string("Select Mode: ") + ModeName(mode)).c_str());
        ops::ConvertSelection(m_Mesh, m_Mode, mode);
        m_Mode = mode;
        SelectionChanged();
    }

    // ---- Undo

    void MeshEditMode::Record(const char* name)
    {
        m_Undo.push_back({ m_Mesh, m_Mode, name ? name : "Edit" });
        while (m_Undo.size() > 64) m_Undo.pop_front();
        m_Redo.clear();
    }

    void MeshEditMode::Commit()
    {
        m_Dirty = true;
        SyncRenderMesh();
    }

    void MeshEditMode::SelectionChanged()
    {
        m_Dirty = true; // (selection is kept in the file, like Blender)
    }

    void MeshEditMode::SyncRenderMesh()
    {
        ++m_GeometryVersion;
        AssetManager::SetMeshData(m_Asset, m_Mesh.ToMeshData());
    }

    void MeshEditMode::Undo()
    {
        if (IsModal())
        {
            EndModal(false);
            return;
        }
        if (m_Undo.empty()) return;
        m_Redo.push_back({ m_Mesh, m_Mode, m_Undo.back().Name });
        m_Mesh = std::move(m_Undo.back().Mesh);
        m_Mode = m_Undo.back().Mode;
        m_Undo.pop_back();
        m_Mesh.Changed();
        Commit();
    }

    void MeshEditMode::Redo()
    {
        if (IsModal() || m_Redo.empty()) return;
        m_Undo.push_back({ m_Mesh, m_Mode, m_Redo.back().Name });
        m_Mesh = std::move(m_Redo.back().Mesh);
        m_Mode = m_Redo.back().Mode;
        m_Redo.pop_back();
        m_Mesh.Changed();
        Commit();
    }

    bool MeshEditMode::GetFrame(glm::vec3& center, float& radius) const
    {
        if (!m_Active) return false;
        std::vector<uint32_t> sel = ops::SelectedVertices(m_Mesh);
        if (sel.empty())
            for (uint32_t v = 0; v < m_Mesh.VertexCount(); ++v)
                if (!m_Mesh.IsVertHidden(v)) sel.push_back(v);
        if (sel.empty()) return false;
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (uint32_t v : sel)
        {
            glm::vec3 w = glm::vec3(m_World * glm::vec4(m_Mesh.Positions[v], 1.0f));
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
        }
        center = (mn + mx) * 0.5f;
        radius = std::max(glm::length(mx - mn) * 0.5f, 0.05f);
        return true;
    }

    // ---- The Scene view

    bool MeshEditMode::UpdateSceneView(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj, bool hovered,
                                       bool focused, bool blocked)
    {
        if (!m_Active) return false;
        m_Origin = origin;
        m_Size = size;
        m_View = view;
        m_Proj = proj;
        UpdateProjection(origin, size, view, proj);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawOverlay(dl);
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        DrawToolbar(origin);
        ImGui::SetCursorScreenPos(cursor);
        DrawMenus();
        m_OverUI = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

        bool owns = false;
        if (IsModal())
        {
            UpdateModal();
            owns = true;
        }
        else
        {
            bool gizmo = GizmoManipulate(origin, size, view, proj);
            if ((hovered || focused) && !ImGui::GetIO().WantTextInput) HandleKeys();
            if (!gizmo) HandleMouse(hovered, blocked);
            owns = gizmo || m_Pressing;
        }
        if (IsModal()) owns = true;

        // The box being dragged.
        if (m_BoxSelecting && m_Pressing)
        {
            ImVec2 a = m_PressPos, b = ImGui::GetMousePos();
            ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
            dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 25));
            dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 180));
        }
        return owns;
    }

    void MeshEditMode::UpdateProjection(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj)
    {
        Scene& scene = m_Ctx.ActiveScene();
        m_World = scene.GetRegistry().Get<TransformComponent>(m_Entity).World;
        m_ViewProj = proj * view;
        glm::mat4 mvp = m_ViewProj * m_World;
        m_CameraWorld = glm::vec3(glm::inverse(view)[3]);
        m_CameraLocal = glm::vec3(glm::inverse(m_World) * glm::vec4(m_CameraWorld, 1.0f));
        uint32_t n = m_Mesh.VertexCount();
        m_Screen.resize(n);
        for (uint32_t v = 0; v < n; ++v)
        {
            glm::vec4 c = mvp * glm::vec4(m_Mesh.Positions[v], 1.0f);
            if (c.w <= 1e-6f)
            {
                m_Screen[v] = ImVec2(kOffscreen, kOffscreen);
                continue;
            }
            glm::vec3 ndc = glm::vec3(c) / c.w;
            m_Screen[v] = ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (1.0f - ndc.y) * 0.5f * size.y);
        }

        // What can be seen (front-most elements, unless X-ray): worked out again when the mesh or the view changes.
        if (m_VisibleVersion == m_GeometryVersion && m_VisibleViewProj == mvp && m_VisibleXRay == m_XRay && m_VertVisible.size() == n) return;
        m_VisibleVersion = m_GeometryVersion;
        m_VisibleViewProj = mvp;
        m_VisibleXRay = m_XRay;
        if (m_RaysVersion != m_GeometryVersion)
        {
            m_Rays.Build(m_Mesh);
            m_RaysVersion = m_GeometryVersion;
        }
        const Topology& t = m_Mesh.Topo();
        const float eps = 2e-3f;
        m_VertVisible.assign(n, 0);
        for (uint32_t v = 0; v < n; ++v)
            m_VertVisible[v] = !m_Mesh.IsVertHidden(v) && OnScreen(m_Screen[v]) && (m_XRay || !m_Rays.Occluded(m_CameraLocal, m_Mesh.Positions[v], eps));
        m_FaceVisible.assign(m_Mesh.FaceCount(), 0);
        for (uint32_t f = 0; f < m_Mesh.FaceCount(); ++f)
        {
            if (m_Mesh.Faces[f].Flags & Hidden) continue;
            bool onScreen = true;
            for (uint32_t v : m_Mesh.Faces[f].Verts) onScreen &= OnScreen(m_Screen[v]);
            if (!onScreen) continue;
            if (m_XRay)
            {
                m_FaceVisible[f] = 1;
                continue;
            }
            glm::vec3 c = m_Mesh.FaceCenter(f), nrm = m_Mesh.FaceNormal(f);
            m_FaceVisible[f] = glm::dot(nrm, m_CameraLocal - c) > 0.0f && !m_Rays.Occluded(m_CameraLocal, c, eps);
        }
        m_EdgeVisible.assign(t.Edges.size(), 0);
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            EdgeKey k = t.Edges[e];
            uint32_t a = EdgeA(k), b = EdgeB(k);
            if (m_Mesh.IsEdgeHidden(k) || m_Mesh.IsVertHidden(a) || m_Mesh.IsVertHidden(b) || !OnScreen(m_Screen[a]) || !OnScreen(m_Screen[b])) continue;
            if (m_XRay || (m_VertVisible[a] && m_VertVisible[b])) m_EdgeVisible[e] = 1;
            else if (m_VertVisible[a] || m_VertVisible[b])
                m_EdgeVisible[e] = !m_Rays.Occluded(m_CameraLocal, (m_Mesh.Positions[a] + m_Mesh.Positions[b]) * 0.5f, eps);
        }
    }

    void MeshEditMode::DrawOverlay(ImDrawList* dl)
    {
        dl->PushClipRect(m_Origin, ImVec2(m_Origin.x + m_Size.x, m_Origin.y + m_Size.y), true);
        const Topology& t = m_Mesh.Topo();
        // Selected faces tinted (without anti-aliased fill edges: neighbouring triangles would leave seams).
        ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        std::vector<uint32_t> tri;
        for (uint32_t f = 0; f < m_Mesh.FaceCount(); ++f)
        {
            if (!(m_Mesh.Faces[f].Flags & Selected) || !m_FaceVisible[f]) continue;
            tri.clear();
            m_Mesh.TriangulateFace(f, tri);
            const auto& verts = m_Mesh.Faces[f].Verts;
            for (size_t i = 0; i + 2 < tri.size(); i += 3)
                dl->AddTriangleFilled(m_Screen[verts[tri[i]]], m_Screen[verts[tri[i + 1]]], m_Screen[verts[tri[i + 2]]], kFaceFill);
        }
        dl->Flags = flags;
        // Edges: selected ones orange; seams red and sharp edges cyan when not selected.
        for (size_t e = 0; e < t.Edges.size(); ++e)
        {
            if (!m_EdgeVisible[e]) continue;
            EdgeKey k = t.Edges[e];
            EdgeData d = m_Mesh.GetEdge(k);
            bool sel = d.Flags & Selected;
            ImU32 col = sel ? kSelected : d.Seam ? kSeam : d.Sharp ? kSharp : (m_XRay ? kWireXRay : kWire);
            dl->AddLine(m_Screen[EdgeA(k)], m_Screen[EdgeB(k)], col, sel || d.Seam || d.Sharp ? 2.0f : 1.25f);
        }
        if (m_Mode == SelectMode::Vertex)
        {
            for (uint32_t v = 0; v < m_Mesh.VertexCount(); ++v)
            {
                if (!m_VertVisible[v]) continue;
                bool sel = m_Mesh.IsVertSelected(v);
                ImVec2 p = m_Screen[v];
                float r = sel ? 3.5f : 3.0f;
                dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), sel ? kSelected : kVertex);
            }
        }
        else if (m_Mode == SelectMode::Face)
        {
            glm::mat4 mvp = m_ViewProj * m_World;
            for (uint32_t f = 0; f < m_Mesh.FaceCount(); ++f)
            {
                if (!m_FaceVisible[f]) continue;
                ImVec2 p = Project(mvp, m_Origin, m_Size, m_Mesh.FaceCenter(f));
                if (!OnScreen(p)) continue;
                bool sel = m_Mesh.Faces[f].Flags & Selected;
                float r = 2.5f;
                dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), sel ? kSelected : kVertex);
            }
        }
        // While moving along an axis, the axis through the pivot.
        if (IsModal() && m_Modal.Axis >= 0)
        {
            glm::vec3 a = AxisVector(m_Modal.Axis);
            static const ImU32 colors[3] = { IM_COL32(230, 70, 70, 220), IM_COL32(110, 200, 60, 220), IM_COL32(70, 120, 240, 220) };
            if (!m_Modal.Plane)
            {
                // (drawn from the pivot's projection outward: far points behind the camera would project badly)
                ImVec2 c = Project(m_ViewProj, m_Origin, m_Size, m_Modal.Pivot);
                ImVec2 c2 = Project(m_ViewProj, m_Origin, m_Size, m_Modal.Pivot + a);
                if (OnScreen(c) && OnScreen(c2))
                {
                    ImVec2 d = Sub(c2, c);
                    float len = Length(d);
                    if (len > 1e-3f)
                    {
                        d = ImVec2(d.x / len * 4000.0f, d.y / len * 4000.0f);
                        dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), colors[m_Modal.Axis], 1.5f);
                    }
                }
            }
        }
        dl->PopClipRect();
    }

    void MeshEditMode::DrawToolbar(const ImVec2& origin)
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 28.0f));
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
        auto toggle = [](const char* label, bool on, const char* tip) {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool pressed = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return pressed;
        };
        std::string modeTips[3] = { "Vertex select (" + Shortcuts::Label("mesh.vertexMode") + ")", "Edge select (" + Shortcuts::Label("mesh.edgeMode") + ")",
                                    "Face select (" + Shortcuts::Label("mesh.faceMode") + ")" };
        for (int i = 0; i < 3; ++i)
        {
            if (i) ImGui::SameLine(0.0f, 2.0f);
            if (toggle(ModeName((SelectMode)i), (int)m_Mode == i, modeTips[i].c_str())) SetSelectMode((SelectMode)i);
        }
        ImGui::SameLine();
        if (toggle("X-Ray", m_XRay, ("See and select through the surface (" + Shortcuts::Label("mesh.xray") + ")").c_str())) m_XRay = !m_XRay;
        ImGui::SameLine();
        if (toggle("Proportional", m_Proportional, ("Moves nearby vertices too, fading with distance (" + Shortcuts::Label("mesh.proportional") +
                                                    "); the mouse wheel sizes it while moving").c_str()))
            m_Proportional = !m_Proportional;
        if (m_Proportional)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            const char* falloffs[] = { "Smooth", "Sphere", "Root", "Linear", "Sharp", "Constant", "Inverse Square" };
            int f = (int)m_Falloff;
            if (ImGui::Combo("##falloff", &f, falloffs, IM_ARRAYSIZE(falloffs))) m_Falloff = (ops::Falloff)f;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            ImGui::DragFloat("##propsize", &m_ProportionalSize, 0.01f, 0.001f, 1000.0f, "%.2f m");
            ImGui::SameLine();
            ImGui::Checkbox("Connected", &m_ProportionalConnected);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(84.0f);
        const char* gizmos[] = { "No Handles", "Move", "Rotate", "Scale" };
        int g = m_Gizmo == ImGuizmo::TRANSLATE ? 1 : m_Gizmo == ImGuizmo::ROTATE ? 2 : m_Gizmo == ImGuizmo::SCALE ? 3 : 0;
        if (ImGui::Combo("##handles", &g, gizmos, 4))
            m_Gizmo = g == 1 ? (int)ImGuizmo::TRANSLATE : g == 2 ? (int)ImGuizmo::ROTATE : g == 3 ? (int)ImGuizmo::SCALE : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Handles on the selection (G, R and S move, rotate and scale with the mouse)");
        ImGui::SameLine();
        if (ImGui::Button("Select")) ImGui::OpenPopup("MeshSelectMenu");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Mesh")) ImGui::OpenPopup("MeshMenu");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Add")) ImGui::OpenPopup("MeshAddMenu");
        ImGui::SameLine();
        if (ImGui::Button("Done")) Exit();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave Edit Mode (%s)", Shortcuts::Label("mesh.editMode").c_str());
        ImGui::PopStyleVar();

        if (m_Active)
        {
            // Counts: selected / all.
            uint32_t sv = 0, se = 0, sf = 0, tris = 0;
            for (uint32_t v = 0; v < m_Mesh.VertexCount(); ++v) sv += m_Mesh.IsVertSelected(v) ? 1 : 0;
            for (EdgeKey k : m_Mesh.Topo().Edges) se += m_Mesh.IsEdgeSelected(k) ? 1 : 0;
            for (const auto& f : m_Mesh.Faces)
            {
                sf += (f.Flags & Selected) ? 1 : 0;
                tris += f.Verts.size() >= 3 ? (uint32_t)f.Verts.size() - 2 : 0;
            }
            ImGui::TextDisabled("Edit Mode: %s   Vertices %u/%u   Edges %u/%u   Faces %u/%u   Triangles %u%s", AssetManager::GetDisplayName(m_Asset).c_str(), sv,
                                m_Mesh.VertexCount(), se, (uint32_t)m_Mesh.Topo().Edges.size(), sf, m_Mesh.FaceCount(), tris, m_Dirty ? "   (unsaved)" : "");
        }
        ImGui::EndGroup();
        m_ToolbarMin = ImGui::GetItemRectMin();
        m_ToolbarMax = ImGui::GetItemRectMax();

        // While transforming: what and how much.
        if (IsModal())
        {
            const char* names[] = { "", "Move", "Rotate", "Scale" };
            std::string text = names[(int)m_Modal.Kind];
            if (m_Modal.Axis >= 0) text += std::string(m_Modal.Plane ? "  plane without " : "  along ") + "XYZ"[m_Modal.Axis] + (m_Modal.LocalAxes ? " (local)" : "");
            if (!m_Modal.Typed.empty()) text += "   [" + m_Modal.Typed + "]";
            if (m_Proportional) text += "   Proportional " + std::to_string(m_ProportionalSize).substr(0, 5) + " m (wheel)";
            text += "   X/Y/Z axis, Shift+axis plane, Ctrl snap, type a value; click or Enter: done, right-click or Esc: cancel";
            ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 8.0f, origin.y + m_Size.y - 24.0f), IM_COL32(255, 255, 255, 230), text.c_str());
        }
    }

    void MeshEditMode::DrawMenus()
    {
        if (m_OpenMenu)
        {
            static const char* ids[] = { "", "MeshDeleteMenu", "MeshMergeMenu", "MeshNormalsMenu", "MeshAddMenu", "MeshEdgeMenu" };
            ImGui::OpenPopup(ids[m_OpenMenu]);
            m_OpenMenu = 0;
        }
        auto deleteItems = [&] {
            if (ImGui::MenuItem("Vertices")) DoDelete(ops::DeleteKind::Vertices);
            if (ImGui::MenuItem("Edges")) DoDelete(ops::DeleteKind::Edges);
            if (ImGui::MenuItem("Faces")) DoDelete(ops::DeleteKind::Faces);
            ImGui::Separator();
            if (ImGui::MenuItem("Only Edges & Faces")) DoDelete(ops::DeleteKind::OnlyEdgesAndFaces);
            if (ImGui::MenuItem("Only Faces")) DoDelete(ops::DeleteKind::OnlyFaces);
        };
        auto mergeItems = [&] {
            if (ImGui::MenuItem("At Center"))
            {
                Record("Merge at Center");
                ops::MergeAtCenter(m_Mesh, false);
                ops::FlushSelection(m_Mesh, m_Mode);
                Commit();
            }
            if (ImGui::MenuItem("Collapse", nullptr, false, true))
            {
                Record("Collapse");
                ops::MergeAtCenter(m_Mesh, true);
                ops::FlushSelection(m_Mesh, m_Mode);
                Commit();
            }
            if (ImGui::MenuItem("By Distance"))
            {
                Record("Merge by Distance");
                size_t removed = ops::MergeByDistance(m_Mesh, m_MergeDistance, true);
                ops::FlushSelection(m_Mesh, m_Mode);
                Commit();
                Log::Info("Merge by distance: removed {} vertices", removed);
            }
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Distance", &m_MergeDistance, 0.0001f, 0.0f, 10.0f, "%.4f m");
        };
        auto normalsItems = [&] {
            if (ImGui::MenuItem("Flip"))
            {
                Record("Flip Normals");
                ops::FlipNormals(m_Mesh);
                Commit();
            }
            if (ImGui::MenuItem("Recalculate Outside", Shortcuts::Label("mesh.recalcNormals").c_str()))
            {
                Record("Recalculate Normals");
                ops::RecalculateNormals(m_Mesh);
                Commit();
            }
            if (ImGui::MenuItem("Recalculate Inside"))
            {
                Record("Recalculate Normals Inside");
                ops::RecalculateNormals(m_Mesh, true);
                Commit();
            }
        };
        auto shadingItems = [&] {
            if (ImGui::MenuItem("Shade Smooth"))
            {
                Record("Shade Smooth");
                ops::SetSmooth(m_Mesh, true);
                Commit();
            }
            if (ImGui::MenuItem("Shade Flat"))
            {
                Record("Shade Flat");
                ops::SetSmooth(m_Mesh, false);
                Commit();
            }
            bool autoSmooth = m_Mesh.AutoSmooth;
            if (ImGui::Checkbox("Auto Smooth", &autoSmooth))
            {
                Record("Auto Smooth");
                m_Mesh.AutoSmooth = autoSmooth;
                Commit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooth faces meeting at a sharper angle than this shade apart");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            float angle = m_Mesh.AutoSmoothAngle;
            if (ImGui::DragFloat("##angle", &angle, 0.5f, 0.0f, 180.0f, "%.0f deg"))
            {
                if (ImGui::IsItemActivated()) Record("Auto Smooth Angle");
                m_Mesh.AutoSmoothAngle = angle;
                Commit();
            }
        };
        auto edgeItems = [&] {
            auto mark = [&](const char* name, auto fn) {
                if (ImGui::MenuItem(name))
                {
                    Record(name);
                    fn();
                    Commit();
                }
            };
            mark("Mark Sharp", [&] { ops::MarkSharp(m_Mesh, true); });
            mark("Clear Sharp", [&] { ops::MarkSharp(m_Mesh, false); });
            mark("Mark Seam", [&] { ops::MarkSeam(m_Mesh, true); });
            mark("Clear Seam", [&] { ops::MarkSeam(m_Mesh, false); });
            static float crease = 1.0f;
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##crease", &crease, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            if (ImGui::Button("Set Crease"))
            {
                Record("Edge Crease");
                ops::SetCrease(m_Mesh, crease);
                Commit();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How sharp subdivision keeps the selected edges (0 smooth, 1 sharp)");
        };
        auto addItems = [&] {
            for (int p = 0; p < (int)Primitive::Count; ++p)
                if (ImGui::MenuItem(PrimitiveName((Primitive)p))) DoAdd((Primitive)p);
        };
        auto selectItems = [&] {
            auto act = [&](const char* name, const char* shortcut, auto fn) {
                if (ImGui::MenuItem(name, shortcut ? Shortcuts::Label(shortcut).c_str() : nullptr))
                {
                    Record(name);
                    fn();
                    SelectionChanged();
                }
            };
            act("All", "mesh.selectAll", [&] { ops::SelectAll(m_Mesh, m_Mode); });
            act("None", "mesh.deselectAll", [&] { ops::DeselectAll(m_Mesh); });
            act("Invert", "mesh.invert", [&] { ops::InvertSelection(m_Mesh, m_Mode); });
            ImGui::Separator();
            act("Linked", "mesh.selectLinked", [&] { ops::SelectLinked(m_Mesh, m_Mode); });
            act("More", "mesh.selectMore", [&] { ops::SelectMore(m_Mesh, m_Mode); });
            act("Less", "mesh.selectLess", [&] { ops::SelectLess(m_Mesh, m_Mode); });
            ImGui::Separator();
            ImGui::TextDisabled("Click: select   Shift+click: toggle   Drag: box (Shift add, Ctrl remove)");
            ImGui::TextDisabled("Alt+click: edge loop   Ctrl+Alt+click: edge ring   L: linked under the mouse");
        };

        if (ImGui::BeginPopup("MeshDeleteMenu"))
        {
            ImGui::TextDisabled("Delete");
            deleteItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MeshMergeMenu"))
        {
            ImGui::TextDisabled("Merge");
            mergeItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MeshNormalsMenu"))
        {
            ImGui::TextDisabled("Normals");
            normalsItems();
            ImGui::Separator();
            shadingItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MeshAddMenu"))
        {
            ImGui::TextDisabled("Add Mesh");
            addItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MeshEdgeMenu"))
        {
            ImGui::TextDisabled("Edge");
            edgeItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MeshSelectMenu"))
        {
            selectItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MeshMenu"))
        {
            if (ImGui::BeginMenu("Transform"))
            {
                if (ImGui::MenuItem("Move", Shortcuts::Label("mesh.grab").c_str())) BeginModal(ModalKind::Move);
                if (ImGui::MenuItem("Rotate", Shortcuts::Label("mesh.rotate").c_str())) BeginModal(ModalKind::Rotate);
                if (ImGui::MenuItem("Scale", Shortcuts::Label("mesh.scale").c_str())) BeginModal(ModalKind::Scale);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Delete"))
            {
                deleteItems();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Merge"))
            {
                mergeItems();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Normals"))
            {
                normalsItems();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Shading"))
            {
                shadingItems();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edges"))
            {
                edgeItems();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Hide Selected", Shortcuts::Label("mesh.hide").c_str()))
            {
                Record("Hide");
                ops::HideSelected(m_Mesh, m_Mode);
                Commit();
            }
            if (ImGui::MenuItem("Hide Unselected", Shortcuts::Label("mesh.hideUnselected").c_str()))
            {
                Record("Hide Unselected");
                ops::HideSelected(m_Mesh, m_Mode, true);
                Commit();
            }
            if (ImGui::MenuItem("Reveal Hidden", Shortcuts::Label("mesh.reveal").c_str()))
            {
                Record("Reveal");
                ops::RevealAll(m_Mesh);
                ops::FlushSelection(m_Mesh, m_Mode);
                Commit();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export as glTF..."))
                if (auto out = FileDialogs::SaveFile({ { L"glTF Binary", L"*.glb" }, { L"glTF", L"*.gltf" } }, L"glb"))
                    if (ExportGltf(m_Mesh.ToMeshData(), *out)) Log::Info("Exported {}", out->string());
            if (ImGui::MenuItem("Save Model", Shortcuts::Label("file.save").c_str())) Save();
            ImGui::EndPopup();
        }
    }

    // ---- Keys

    void MeshEditMode::HandleKeys()
    {
        using Shortcuts::Pressed;
        auto selectOp = [&](const char* name, auto fn) {
            Record(name);
            fn();
            SelectionChanged();
        };
        if (Pressed("mesh.vertexMode")) SetSelectMode(SelectMode::Vertex);
        if (Pressed("mesh.edgeMode")) SetSelectMode(SelectMode::Edge);
        if (Pressed("mesh.faceMode")) SetSelectMode(SelectMode::Face);
        if (Pressed("mesh.selectAll")) selectOp("Select All", [&] { ops::SelectAll(m_Mesh, m_Mode); });
        if (Pressed("mesh.deselectAll")) selectOp("Select None", [&] { ops::DeselectAll(m_Mesh); });
        if (Pressed("mesh.invert")) selectOp("Invert Selection", [&] { ops::InvertSelection(m_Mesh, m_Mode); });
        if (Pressed("mesh.selectLinked")) selectOp("Select Linked", [&] { ops::SelectLinked(m_Mesh, m_Mode); });
        if (Pressed("mesh.selectMore") || (ImGui::GetIO().KeyMods == ImGuiMod_Ctrl && ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)))
            selectOp("Select More", [&] { ops::SelectMore(m_Mesh, m_Mode); });
        if (Pressed("mesh.selectLess") || (ImGui::GetIO().KeyMods == ImGuiMod_Ctrl && ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)))
            selectOp("Select Less", [&] { ops::SelectLess(m_Mesh, m_Mode); });
        if (Pressed("mesh.linkedPick"))
        {
            // Everything joined to the element under the mouse.
            ImVec2 mouse = ImGui::GetMousePos();
            int64_t v = NearestVertex(mouse, 40.0f);
            if (v < 0)
                if (int64_t f = FaceUnderMouse(mouse); f >= 0) v = m_Mesh.Faces[(size_t)f].Verts[0];
            if (v >= 0) selectOp("Select Linked", [&] { ops::SelectLinked(m_Mesh, m_Mode, v); });
        }
        if (Pressed("mesh.grab")) BeginModal(ModalKind::Move);
        if (Pressed("mesh.rotate")) BeginModal(ModalKind::Rotate);
        if (Pressed("mesh.scale")) BeginModal(ModalKind::Scale);
        if (Pressed("mesh.delete") || Pressed("edit.delete")) m_OpenMenu = 1;
        if (Pressed("mesh.merge")) m_OpenMenu = 2;
        if (Pressed("mesh.normals")) m_OpenMenu = 3;
        if (Pressed("mesh.add")) m_OpenMenu = 4;
        if (Pressed("mesh.edgeMenu")) m_OpenMenu = 5;
        if (Pressed("mesh.recalcNormals"))
        {
            Record("Recalculate Normals");
            ops::RecalculateNormals(m_Mesh);
            Commit();
        }
        if (Pressed("mesh.hide"))
        {
            Record("Hide");
            ops::HideSelected(m_Mesh, m_Mode);
            Commit();
        }
        if (Pressed("mesh.hideUnselected"))
        {
            Record("Hide Unselected");
            ops::HideSelected(m_Mesh, m_Mode, true);
            Commit();
        }
        if (Pressed("mesh.reveal"))
        {
            Record("Reveal");
            ops::RevealAll(m_Mesh);
            ops::FlushSelection(m_Mesh, m_Mode);
            Commit();
        }
        if (Pressed("mesh.proportional")) m_Proportional = !m_Proportional;
        if (Pressed("mesh.xray")) m_XRay = !m_XRay;
    }

    // ---- Mouse selection

    int64_t MeshEditMode::NearestVertex(const ImVec2& mouse, float maxPixels) const
    {
        int64_t best = -1;
        float bestD = maxPixels;
        for (uint32_t v = 0; v < m_Mesh.VertexCount() && v < m_VertVisible.size(); ++v)
        {
            if (!m_VertVisible[v]) continue;
            float d = Length(Sub(m_Screen[v], mouse));
            if (d < bestD)
            {
                bestD = d;
                best = v;
            }
        }
        return best;
    }

    int64_t MeshEditMode::NearestEdge(const ImVec2& mouse, float maxPixels) const
    {
        const Topology& t = m_Mesh.Topo();
        int64_t best = -1;
        float bestD = maxPixels;
        for (size_t e = 0; e < t.Edges.size() && e < m_EdgeVisible.size(); ++e)
        {
            if (!m_EdgeVisible[e]) continue;
            float d = SegmentDistance(mouse, m_Screen[EdgeA(t.Edges[e])], m_Screen[EdgeB(t.Edges[e])]);
            if (d < bestD)
            {
                bestD = d;
                best = (int64_t)e;
            }
        }
        return best;
    }

    int64_t MeshEditMode::FaceUnderMouse(const ImVec2& mouse) const
    {
        if (m_XRay)
        {
            // Through the surface: the nearest face center on screen.
            glm::mat4 mvp = m_ViewProj * m_World;
            int64_t best = -1;
            float bestD = 18.0f;
            for (uint32_t f = 0; f < m_Mesh.FaceCount() && f < m_FaceVisible.size(); ++f)
            {
                if (!m_FaceVisible[f]) continue;
                float d = Length(Sub(Project(mvp, m_Origin, m_Size, m_Mesh.FaceCenter(f)), mouse));
                if (d < bestD)
                {
                    bestD = d;
                    best = f;
                }
            }
            if (best >= 0) return best;
        }
        glm::vec3 from, dir;
        RayAt(m_ViewProj, m_Origin, m_Size, mouse, from, dir);
        glm::mat4 inv = glm::inverse(m_World);
        glm::vec3 a = glm::vec3(inv * glm::vec4(from, 1.0f)), b = glm::vec3(inv * glm::vec4(from + dir, 1.0f));
        auto hit = m_Rays.Raycast(a, b - a);
        return hit.Hit ? (int64_t)hit.Face : -1;
    }

    void MeshEditMode::ClickSelect(const ImVec2& mouse, bool extend)
    {
        int64_t hit = -1;
        switch (m_Mode)
        {
        case SelectMode::Vertex: hit = NearestVertex(mouse, 16.0f); break;
        case SelectMode::Edge: hit = NearestEdge(mouse, 12.0f); break;
        case SelectMode::Face: hit = FaceUnderMouse(mouse); break;
        }
        if (hit < 0)
        {
            if (!extend && ops::AnySelected(m_Mesh))
            {
                Record("Select None");
                ops::DeselectAll(m_Mesh);
                SelectionChanged();
            }
            return;
        }
        Record("Select");
        const Topology& t = m_Mesh.Topo();
        bool was = false;
        switch (m_Mode)
        {
        case SelectMode::Vertex: was = m_Mesh.IsVertSelected((uint32_t)hit); break;
        case SelectMode::Edge: was = m_Mesh.IsEdgeSelected(t.Edges[(size_t)hit]); break;
        case SelectMode::Face: was = (m_Mesh.Faces[(size_t)hit].Flags & Selected) != 0; break;
        }
        if (!extend) ops::DeselectAll(m_Mesh);
        bool on = !(extend && was);
        switch (m_Mode)
        {
        case SelectMode::Vertex: ops::SelectVertex(m_Mesh, (uint32_t)hit, on); break;
        case SelectMode::Edge: ops::SelectEdge(m_Mesh, m_Mesh.Topo().Edges[(size_t)hit], on); break;
        case SelectMode::Face: ops::SelectFace(m_Mesh, (uint32_t)hit, on); break;
        }
        ops::FlushSelection(m_Mesh, m_Mode);
        SelectionChanged();
    }

    void MeshEditMode::BoxSelect(const ImVec2& a, const ImVec2& b, int op)
    {
        ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
        auto inside = [&](ImVec2 p) { return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y; };
        Record("Box Select");
        if (op == 0) ops::DeselectAll(m_Mesh);
        bool on = op != 2;
        const Topology& t = m_Mesh.Topo();
        switch (m_Mode)
        {
        case SelectMode::Vertex:
            for (uint32_t v = 0; v < m_Mesh.VertexCount(); ++v)
                if (m_VertVisible[v] && inside(m_Screen[v])) ops::SelectVertex(m_Mesh, v, on);
            break;
        case SelectMode::Edge:
        {
            std::vector<EdgeKey> picked;
            for (size_t e = 0; e < t.Edges.size(); ++e)
                if (m_EdgeVisible[e] && inside(m_Screen[EdgeA(t.Edges[e])]) && inside(m_Screen[EdgeB(t.Edges[e])])) picked.push_back(t.Edges[e]);
            for (EdgeKey k : picked) ops::SelectEdge(m_Mesh, k, on);
            break;
        }
        case SelectMode::Face:
        {
            glm::mat4 mvp = m_ViewProj * m_World;
            for (uint32_t f = 0; f < m_Mesh.FaceCount(); ++f)
                if (m_FaceVisible[f] && inside(Project(mvp, m_Origin, m_Size, m_Mesh.FaceCenter(f)))) ops::SelectFace(m_Mesh, f, on);
            break;
        }
        }
        ops::FlushSelection(m_Mesh, m_Mode);
        SelectionChanged();
    }

    void MeshEditMode::HandleMouse(bool hovered, bool blocked)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        bool overToolbar = mouse.x >= m_ToolbarMin.x && mouse.x <= m_ToolbarMax.x && mouse.y >= m_ToolbarMin.y && mouse.y <= m_ToolbarMax.y;
        if (hovered && !blocked && !m_OverUI && !overToolbar && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_Pressing = true;
            m_PressAlt = io.KeyAlt;
            m_PressPos = mouse;
            m_BoxSelecting = false;
        }
        if (!m_Pressing) return;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (!m_PressAlt && Length(Sub(mouse, m_PressPos)) > 4.0f) m_BoxSelecting = true;
            return;
        }
        // Released.
        m_Pressing = false;
        if (m_PressAlt)
        {
            // Alt+click (not an Alt+drag orbit): the loop (or ring, with Ctrl) through the nearest edge.
            if (Length(Sub(mouse, m_PressPos)) < 4.0f)
                if (int64_t e = NearestEdge(mouse, 30.0f); e >= 0)
                {
                    Record(io.KeyCtrl ? "Select Ring" : "Select Loop");
                    ops::SelectLoop(m_Mesh, m_Mode, (uint32_t)e, io.KeyShift, io.KeyCtrl);
                    SelectionChanged();
                }
        }
        else if (m_BoxSelecting) BoxSelect(m_PressPos, mouse, io.KeyShift ? 1 : io.KeyCtrl ? 2 : 0);
        else ClickSelect(mouse, io.KeyShift);
        m_BoxSelecting = false;
    }

    // ---- Moving, rotating and scaling with the mouse (G, R, S)

    glm::vec3 MeshEditMode::AxisVector(int axis) const
    {
        glm::vec3 a(0.0f);
        if (axis < 0 || axis > 2) return a;
        a[axis] = 1.0f;
        if (m_Modal.LocalAxes)
        {
            glm::vec3 local = glm::vec3(m_World[axis]);
            float len = glm::length(local);
            if (len > 1e-8f) a = local / len;
        }
        return a;
    }

    void MeshEditMode::BeginModal(ModalKind kind)
    {
        if (!m_Active || ops::SelectedVertices(m_Mesh).empty()) return;
        const char* names[] = { "", "Move", "Rotate", "Scale" };
        Record(names[(int)kind]);
        m_Modal = {};
        m_Modal.Kind = kind;
        m_Modal.StartFrame = ImGui::GetFrameCount();
        m_Modal.Start = m_Mesh.Positions;
        m_Modal.Pivot = glm::vec3(m_World * glm::vec4(ops::SelectionCenter(m_Mesh), 1.0f));
        m_Modal.MouseStart = ImGui::GetMousePos();
        m_Modal.PivotScreen = Project(m_ViewProj, m_Origin, m_Size, m_Modal.Pivot);
        ImVec2 d = Sub(m_Modal.MouseStart, m_Modal.PivotScreen);
        m_Modal.LastScreenAngle = std::atan2(d.y, d.x);
        // Proportional weights (the radius is in world units; the mesh may be scaled).
        float scale = (glm::length(glm::vec3(m_World[0])) + glm::length(glm::vec3(m_World[1])) + glm::length(glm::vec3(m_World[2]))) / 3.0f;
        if (m_Proportional) m_Modal.Weights = ops::ProportionalWeights(m_Mesh, m_ProportionalSize / std::max(scale, 1e-6f), m_Falloff, m_ProportionalConnected);
        else
        {
            m_Modal.Weights.assign(m_Mesh.VertexCount(), 0.0f);
            for (uint32_t v : ops::SelectedVertices(m_Mesh)) m_Modal.Weights[v] = 1.0f;
        }
    }

    void MeshEditMode::EndModal(bool confirm)
    {
        if (!IsModal()) return;
        if (!confirm)
        {
            m_Mesh.Positions = m_Modal.Start;
            if (!m_Undo.empty()) m_Undo.pop_back(); // nothing happened
            SyncRenderMesh();
        }
        else Commit();
        m_Modal = {};
    }

    void MeshEditMode::UpdateModal()
    {
        ImGuiIO& io = ImGui::GetIO();
        Modal& md = m_Modal;
        bool first = ImGui::GetFrameCount() == md.StartFrame; // (the click or key that started it is not for it)
        // Finish or cancel.
        if (!first && (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        {
            EndModal(false);
            return;
        }
        bool confirm = !first && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
                       ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        // Switch between move, rotate and scale.
        ModalKind switchTo = ModalKind::None;
        if (!first && ImGui::IsKeyPressed(ImGuiKey_G) && md.Kind != ModalKind::Move) switchTo = ModalKind::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_R) && md.Kind != ModalKind::Rotate) switchTo = ModalKind::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_S) && md.Kind != ModalKind::Scale) switchTo = ModalKind::Scale;
        if (switchTo != ModalKind::None)
        {
            md.Kind = switchTo;
            md.MouseStart = io.MousePos;
            ImVec2 d = Sub(md.MouseStart, md.PivotScreen);
            md.LastScreenAngle = std::atan2(d.y, d.x);
            md.Angle = 0.0f;
            md.Typed.clear();
            m_Mesh.Positions = md.Start;
        }
        // Axis constraints: X/Y/Z (again: the object's own axes, a third time: none), Shift for the plane.
        for (int i = 0; i < 3; ++i)
        {
            ImGuiKey key = (ImGuiKey)(ImGuiKey_X + i);
            if (!ImGui::IsKeyPressed(key, false)) continue;
            bool plane = io.KeyShift;
            if (md.Axis == i && md.Plane == plane)
            {
                if (!md.LocalAxes) md.LocalAxes = true;
                else
                {
                    md.Axis = -1;
                    md.LocalAxes = false;
                }
            }
            else
            {
                md.Axis = i;
                md.Plane = plane;
                md.LocalAxes = false;
            }
        }
        // Typed values.
        for (int k = 0; k <= 9; ++k)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + k)) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + k))) md.Typed += (char)('0' + k);
        if ((ImGui::IsKeyPressed(ImGuiKey_Period) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal)) && md.Typed.find('.') == std::string::npos) md.Typed += '.';
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            md.Typed = !md.Typed.empty() && md.Typed[0] == '-' ? md.Typed.substr(1) : "-" + md.Typed;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !md.Typed.empty()) md.Typed.pop_back();
        float typed = 0.0f;
        bool hasTyped = false;
        if (!md.Typed.empty() && md.Typed != "-" && md.Typed != "." && md.Typed != "-.")
        {
            typed = std::strtof(md.Typed.c_str(), nullptr);
            hasTyped = true;
        }
        // The proportional size follows the mouse wheel.
        if (m_Proportional && io.MouseWheel != 0.0f)
        {
            m_ProportionalSize = std::clamp(m_ProportionalSize * (1.0f + 0.1f * io.MouseWheel), 0.001f, 1000.0f);
            std::vector<glm::vec3> now = m_Mesh.Positions;
            m_Mesh.Positions = md.Start; // weights measured where things started
            float scale = (glm::length(glm::vec3(m_World[0])) + glm::length(glm::vec3(m_World[1])) + glm::length(glm::vec3(m_World[2]))) / 3.0f;
            md.Weights = ops::ProportionalWeights(m_Mesh, m_ProportionalSize / std::max(scale, 1e-6f), m_Falloff, m_ProportionalConnected);
            m_Mesh.Positions = std::move(now);
        }

        const auto& prefs = EditorPreferences::Get();
        bool snap = io.KeyCtrl != prefs.Snap;
        ImVec2 mouse = io.MousePos;
        glm::vec3 axis = AxisVector(md.Axis);
        glm::vec3 toViewer = glm::normalize(m_CameraWorld - md.Pivot);
        glm::mat4 inv = glm::inverse(m_World);

        glm::vec3 move(0.0f);
        glm::mat3 linear(1.0f); // rotation or scale about the pivot
        float angle = 0.0f;
        glm::vec3 rotAxis = md.Axis >= 0 && !md.Plane ? axis : toViewer;
        switch (md.Kind)
        {
        case ModalKind::Move:
        {
            if (hasTyped) move = (md.Axis >= 0 && !md.Plane ? axis : glm::vec3(1, 0, 0)) * typed; // (no axis: along X, as Blender)
            else
            {
                glm::vec3 f0, d0, f1, d1;
                RayAt(m_ViewProj, m_Origin, m_Size, md.MouseStart, f0, d0);
                RayAt(m_ViewProj, m_Origin, m_Size, mouse, f1, d1);
                if (md.Axis >= 0 && !md.Plane)
                {
                    float s = ClosestOnAxis(f1, d1, md.Pivot, axis) - ClosestOnAxis(f0, d0, md.Pivot, axis);
                    if (snap) s = Snap(s, prefs.SnapMove);
                    move = axis * s;
                }
                else
                {
                    glm::vec3 normal = md.Axis >= 0 ? axis : -glm::vec3(glm::inverse(m_View)[2]);
                    glm::vec3 h0, h1;
                    if (RayPlane(f0, d0, md.Pivot, normal, h0) && RayPlane(f1, d1, md.Pivot, normal, h1)) move = h1 - h0;
                    if (snap)
                        for (int i = 0; i < 3; ++i) move[i] = Snap(move[i], prefs.SnapMove);
                }
            }
            break;
        }
        case ModalKind::Rotate:
        {
            ImVec2 d = Sub(mouse, md.PivotScreen);
            float a = std::atan2(d.y, d.x);
            float step = a - md.LastScreenAngle;
            while (step > glm::pi<float>()) step -= glm::two_pi<float>();
            while (step < -glm::pi<float>()) step += glm::two_pi<float>();
            md.Angle += step;
            md.LastScreenAngle = a;
            // Screen y points down: turning clockwise on screen turns counter-clockwise about an axis facing away.
            float sign = glm::dot(rotAxis, toViewer) >= 0.0f ? -1.0f : 1.0f;
            angle = sign * md.Angle;
            if (hasTyped) angle = glm::radians(typed);
            else if (snap) angle = glm::radians(Snap(glm::degrees(angle), prefs.SnapRotate));
            break;
        }
        case ModalKind::Scale:
        {
            float start = std::max(Length(Sub(md.MouseStart, md.PivotScreen)), 1.0f);
            float f = Length(Sub(mouse, md.PivotScreen)) / start;
            if (hasTyped) f = typed;
            else if (snap) f = Snap(f, prefs.SnapScale);
            if (md.Axis < 0) linear = glm::mat3(f);
            else if (!md.Plane) linear = glm::mat3(1.0f) + (f - 1.0f) * glm::outerProduct(axis, axis);
            else linear = glm::mat3(f) + (1.0f - f) * glm::outerProduct(axis, axis);
            break;
        }
        default: break;
        }

        for (uint32_t v = 0; v < m_Mesh.VertexCount(); ++v)
        {
            float w = v < md.Weights.size() ? md.Weights[v] : 0.0f;
            if (w <= 0.0f || m_Mesh.IsVertHidden(v))
            {
                m_Mesh.Positions[v] = md.Start[v];
                continue;
            }
            glm::vec3 p = glm::vec3(m_World * glm::vec4(md.Start[v], 1.0f));
            switch (md.Kind)
            {
            case ModalKind::Move: p += move * w; break;
            case ModalKind::Rotate: p = md.Pivot + glm::mat3(glm::rotate(glm::mat4(1.0f), angle * w, rotAxis)) * (p - md.Pivot); break;
            case ModalKind::Scale: p = md.Pivot + (glm::mat3(1.0f) + (linear - glm::mat3(1.0f)) * w) * (p - md.Pivot); break;
            default: break;
            }
            m_Mesh.Positions[v] = glm::vec3(inv * glm::vec4(p, 1.0f));
        }
        SyncRenderMesh();
        if (confirm) EndModal(true);
    }

    bool MeshEditMode::GizmoManipulate(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj)
    {
        static bool wasUsing = false;
        static glm::mat4 last(1.0f);
        if (m_Gizmo == 0 || ops::SelectedVertices(m_Mesh).empty())
        {
            wasUsing = false;
            return false;
        }
        glm::vec3 center = glm::vec3(m_World * glm::vec4(ops::SelectionCenter(m_Mesh), 1.0f));
        glm::mat4 matrix = wasUsing ? last : glm::translate(glm::mat4(1.0f), center);
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
        glm::mat4 before = matrix;
        const auto& prefs = EditorPreferences::Get();
        bool snap = prefs.Snap != ImGui::GetIO().KeyCtrl;
        float snapValue = m_Gizmo == ImGuizmo::ROTATE ? prefs.SnapRotate : m_Gizmo == ImGuizmo::SCALE ? prefs.SnapScale : prefs.SnapMove;
        float snaps[3] = { snapValue, snapValue, snapValue };
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), (ImGuizmo::OPERATION)m_Gizmo, ImGuizmo::WORLD, glm::value_ptr(matrix), nullptr,
                             snap ? snaps : nullptr);
        bool using_ = ImGuizmo::IsUsing();
        if (using_ && !wasUsing) Record(m_Gizmo == ImGuizmo::TRANSLATE ? "Move" : m_Gizmo == ImGuizmo::ROTATE ? "Rotate" : "Scale");
        if (using_ && matrix != before)
        {
            // The change since last frame, about the pivot, applied in the model's space.
            glm::mat4 delta = matrix * glm::inverse(before);
            glm::mat4 local = glm::inverse(m_World) * delta * m_World;
            std::vector<float> weights;
            if (m_Proportional)
            {
                float scale = (glm::length(glm::vec3(m_World[0])) + glm::length(glm::vec3(m_World[1])) + glm::length(glm::vec3(m_World[2]))) / 3.0f;
                weights = ops::ProportionalWeights(m_Mesh, m_ProportionalSize / std::max(scale, 1e-6f), m_Falloff, m_ProportionalConnected);
            }
            ops::TransformVertices(m_Mesh, local, m_Proportional ? &weights : nullptr);
            SyncRenderMesh();
        }
        if (!using_ && wasUsing) Commit();
        wasUsing = using_;
        last = matrix;
        return using_ || ImGuizmo::IsOver();
    }

    // ---- Operations

    void MeshEditMode::DoDelete(ops::DeleteKind kind)
    {
        Record("Delete");
        ops::Delete(m_Mesh, kind);
        ops::FlushSelection(m_Mesh, m_Mode);
        Commit();
    }

    void MeshEditMode::DoAdd(Primitive p)
    {
        Record((std::string("Add ") + PrimitiveName(p)).c_str());
        ops::DeselectAll(m_Mesh);
        m_Mesh.Append(MakePrimitive(p));
        ops::FlushSelection(m_Mesh, m_Mode);
        Commit();
    }

    // ---- Making an editable copy of another mesh

    void MeshEditMode::DrawPopups()
    {
        if (m_OpenConvert)
        {
            ImGui::OpenPopup("Make Model Editable");
            m_OpenConvert = false;
        }
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Make Model Editable", nullptr, ImGuiWindowFlags_NoResize)) return;
        Scene& scene = m_Ctx.ActiveScene();
        bool valid = scene.IsValid(m_ConvertEntity) && scene.GetRegistry().Has<MeshRendererComponent>(m_ConvertEntity);
        AssetHandle source = valid ? scene.GetRegistry().Get<MeshRendererComponent>(m_ConvertEntity).Mesh : AssetHandle::None();
        std::string meshName = AssetManager::IsBuiltin(source) ? scene.GetRegistry().Get<TagComponent>(m_ConvertEntity).Name : AssetManager::GetDisplayName(source);
        ImGui::TextWrapped("'%s' is not a model made in the editor. Edit Mode works on an editable copy: a new model in Assets/Models that "
                           "this object uses from now on (the original stays as it is).",
                           meshName.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Make Editable Copy", ImVec2(160.0f, 0.0f)) && valid)
        {
            auto data = AssetManager::GetMeshData(source);
            if (data)
            {
                EditableMesh mesh = EditableMesh::FromMeshData(*data);
                // Imported models keep their material slots' materials.
                const std::vector<AssetHandle> materials = AssetManager::IsBuiltin(source) ? std::vector<AssetHandle>{} : AssetManager::GetMeshMaterials(source);
                std::string name = meshName;
                std::erase_if(name, [](char c) { return std::strchr("<>:\"/\\|?*", c) != nullptr; });
                AssetHandle model = CreateModelAsset(mesh, AssetManager::GetAssetsDirectory() / "Models", name.empty() ? "Model" : name);
                if (model.IsValid())
                {
                    m_Ctx.RecordUndo("Make Model Editable");
                    auto& mr = scene.GetRegistry().Get<MeshRendererComponent>(m_ConvertEntity);
                    mr.Mesh = model;
                    if (mr.Materials.size() < materials.size()) mr.Materials.resize(materials.size(), AssetHandle::None());
                    for (size_t i = 0; i < materials.size(); ++i)
                        if (!mr.Materials[i].IsValid() && !AssetManager::IsSubAsset(materials[i])) mr.Materials[i] = materials[i];
                    m_Ctx.MarkDirty();
                    Log::Info("Made an editable copy: {}", AssetManager::GetMetadata(model)->Path.generic_string());
                    m_Ctx.Select(m_ConvertEntity);
                    Enter(m_ConvertEntity);
                }
            }
            else Log::Error("Could not read the mesh to copy it");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
