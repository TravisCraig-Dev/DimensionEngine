#include "MeshEditMode.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Scene/Components.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Edit Mode's UV editing: the U menu and the UV Editor window.
namespace Editor
{
    using namespace Dimension::Modeling;
    namespace uv = Dimension::Modeling::UV;
    namespace pt = Dimension::Modeling::Paint;

    namespace
    {
        constexpr ImU32 kUVSelected = IM_COL32(255, 160, 40, 255);
        constexpr ImU32 kUVWire = IM_COL32(225, 225, 225, 210);
        constexpr ImU32 kUVSeam = IM_COL32(235, 70, 50, 255);
        constexpr ImU32 kUVFaceSelected = IM_COL32(255, 160, 40, 55);
        constexpr ImU32 kUVPin = IM_COL32(235, 40, 40, 255);
        constexpr ImU32 kUVVertex = IM_COL32(20, 20, 20, 255);

        float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

        float SegmentDist(ImVec2 p, ImVec2 a, ImVec2 b)
        {
            ImVec2 ab(b.x - a.x, b.y - a.y);
            float len2 = ab.x * ab.x + ab.y * ab.y;
            float t = len2 > 0.0f ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
            return Dist(p, ImVec2(a.x + ab.x * t, a.y + ab.y * t));
        }

        bool InTriangle(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c)
        {
            auto cross = [](ImVec2 o, ImVec2 u, ImVec2 v) { return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x); };
            float d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
            bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            return !(neg && pos);
        }

        // Blue (true to the surface) through green and yellow to red (badly stretched).
        ImU32 StretchColor(float s)
        {
            float h = (1.0f - std::clamp(s, 0.0f, 1.0f)) * 0.667f, r, g, b;
            ImGui::ColorConvertHSVtoRGB(h, 0.85f, 0.9f, r, g, b);
            return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 150);
        }

        glm::vec2 CornerUV(const Face& f, size_t c) { return c < f.UVs.size() ? f.UVs[c] : glm::vec2(0.0f); }

        const char* kUVModeNames[] = { "Vertex", "Edge", "Face", "Island" };
    }

    // ---- The UV menu (U)

    void MeshEditMode::UVMenuItems(bool inUVEditor)
    {
        std::vector<uint32_t> faces = uv::SelectedFaces(m_Mesh);
        bool any = !faces.empty();
        if (!any) ImGui::TextDisabled("Select faces to work on");
        if (ImGui::MenuItem("Unwrap", nullptr, false, any))
            UVOperate("Unwrap", [&] {
                int charts = uv::Unwrap(m_Mesh, faces, m_Unwrap);
                Log::Info("Unwrapped {} faces into {} pieces", faces.size(), charts);
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flattens the faces, cut apart along seams (Mark Seam) and where the selection ends");
        {
            ImGui::Indent();
            const char* methods[] = { "Angle Based", "Conformal", "Minimum Stretch" };
            int method = (int)m_Unwrap.Method;
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("Method", &method, methods, 3)) m_Unwrap.Method = (uv::UnwrapMethod)method;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Angle Based solves the flat angles closest to the surface's, then lays the faces out from them; "
                                  "Conformal keeps angles only (fastest, sizes may grow apart); Minimum Stretch refines a "
                                  "conformal map towards keeping each face's shape and size");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Margin", &m_Unwrap.Margin, 0.001f, 0.0f, 0.25f, "%.3f");
            ImGui::Checkbox("Use Pins", &m_Unwrap.UsePins);
            ImGui::Unindent();
        }
        if (ImGui::MenuItem("Smart UV Project", nullptr, false, any))
            UVOperate("Smart UV Project", [&] {
                int charts = uv::SmartProject(m_Mesh, faces, m_SmartAngle, m_Unwrap.Margin);
                Log::Info("Smart UV Project: {} pieces", charts);
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cuts the faces by the way they face and projects each piece flat (no seams needed)");
        ImGui::Indent();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Angle Limit", &m_SmartAngle, 0.5f, 1.0f, 89.0f, "%.0f deg");
        ImGui::Unindent();
        if (ImGui::MenuItem("Cube Projection", nullptr, false, any)) UVOperate("Cube Projection", [&] { uv::CubeProject(m_Mesh, faces, m_CubeSize); });
        ImGui::Indent();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Cube Size", &m_CubeSize, 0.01f, 0.001f, 1000.0f, "%.2f m");
        ImGui::Unindent();
        if (ImGui::MenuItem("Cylinder Projection", nullptr, false, any))
            UVOperate("Cylinder Projection", [&] { uv::CylinderProject(m_Mesh, faces, m_ProjectAxis); });
        if (ImGui::MenuItem("Sphere Projection", nullptr, false, any)) UVOperate("Sphere Projection", [&] { uv::SphereProject(m_Mesh, faces, m_ProjectAxis); });
        ImGui::Indent();
        const char* axes[] = { "X", "Y", "Z" };
        ImGui::SetNextItemWidth(120.0f);
        ImGui::Combo("Axis", &m_ProjectAxis, axes, 3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The object's own axis the cylinder or sphere wraps around");
        ImGui::Unindent();
        if (ImGui::MenuItem("Project From View", nullptr, false, any))
            UVOperate("Project From View", [&] {
                glm::mat4 toClip = m_Proj * m_View * m_World;
                uv::ProjectFromView(m_Mesh, faces, toClip, m_Size.y > 0.0f ? m_Size.x / m_Size.y : 1.0f, true);
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("As the Scene view sees them, stretched to fill the texture");
        if (ImGui::MenuItem("Reset", nullptr, false, any)) UVOperate("Reset UVs", [&] { uv::Reset(m_Mesh, faces); });
        if (ImGui::BeginMenu("Follow Active Quads", any))
        {
            if (ImGui::MenuItem("Lay Out"))
                UVOperate("Follow Active Quads", [&] { Log::Info("Laid out {} quads", uv::FollowActiveQuads(m_Mesh, faces, -1, m_FollowEven)); });
            ImGui::Checkbox("Even Steps", &m_FollowEven);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every row and column the same size (else as long as the edges on the surface)");
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lays the selected quads out as a grid continuing the active quad's UVs (the first fully UV-selected quad)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each face fills the whole texture");
        ImGui::Separator();
        if (!inUVEditor)
        {
            if (ImGui::MenuItem("Mark Seam")) UVOperate("Mark Seam", [&] { Ops::MarkSeam(m_Mesh, true); });
            if (ImGui::MenuItem("Clear Seam")) UVOperate("Clear Seam", [&] { Ops::MarkSeam(m_Mesh, false); });
        }
        if (ImGui::MenuItem("Seams From Islands", nullptr, false, any))
            UVOperate("Seams From Islands", [&] { Log::Info("Marked {} seams", uv::SeamsFromIslands(m_Mesh, faces)); });
        ImGui::Separator();
        if (ImGui::MenuItem("Pack Islands", nullptr, false, any)) UVOperate("Pack Islands", [&] { uv::PackIslands(m_Mesh, faces, m_Unwrap.Margin); });
        if (ImGui::MenuItem("Average Islands Scale", nullptr, false, any)) UVOperate("Average Islands Scale", [&] { uv::AverageIslandScale(m_Mesh, faces); });
        if (ImGui::MenuItem("Minimize Stretch", nullptr, false, any)) UVOperate("Minimize Stretch", [&] { uv::Relax(m_Mesh, faces, 20); });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Relaxes the islands with selected UVs (all when none is) towards the surface's true shape");
        if (inUVEditor)
        {
            ImGui::Separator();
            if (m_UVSync) faces = UVFaces();
            bool sel = !uv::SelectedCorners(m_Mesh, faces).empty();
            if (ImGui::MenuItem("Pin", Shortcuts::Label("uv.pin").c_str(), false, sel)) UVOperate("Pin", [&] { uv::SetPinned(m_Mesh, faces, true); });
            if (ImGui::MenuItem("Unpin", Shortcuts::Label("uv.unpin").c_str(), false, sel)) UVOperate("Unpin", [&] { uv::SetPinned(m_Mesh, faces, false); });
            if (ImGui::BeginMenu("Merge", sel))
            {
                if (ImGui::MenuItem("At Center")) UVOperate("Merge UVs", [&] { uv::WeldSelected(m_Mesh, faces); });
                if (ImGui::MenuItem("By Distance"))
                    UVOperate("Merge UVs by Distance", [&] { Log::Info("Merged {} UVs", uv::MergeByDistance(m_Mesh, faces, m_UVMergeDistance)); });
                ImGui::SetNextItemWidth(100.0f);
                ImGui::DragFloat("Distance", &m_UVMergeDistance, 0.0001f, 0.0f, 0.5f, "%.4f");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Split Selection", nullptr, false, sel)) UVOperate("Split UV Selection", [&] { uv::SplitSelection(m_Mesh, faces); });
            if (ImGui::BeginMenu("Stitch", sel))
            {
                if (ImGui::MenuItem("Stitch", Shortcuts::Label("uv.stitch").c_str()))
                    UVOperate("Stitch UVs", [&] { Log::Info("Stitched {} vertices", uv::Stitch(m_Mesh, faces, m_StitchSnap)); });
                ImGui::Checkbox("Snap Islands", &m_StitchSnap);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves (and turns) islands onto the one they are stitched to, else the UVs meet halfway");
                ImGui::EndMenu();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Joins selected UVs of the same vertex that lie apart (closing a seam between islands)");
            if (ImGui::MenuItem("Snap to Pixels", nullptr, false, sel))
                UVOperate("Snap UVs to Pixels", [&] { uv::SnapToPixels(m_Mesh, faces, UVTextureSize()); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps only whole faces selected, so moving them tears them off their neighbours");
            if (ImGui::BeginMenu("Align", sel))
            {
                if (ImGui::MenuItem("Vertically (same U)")) UVOperate("Align UVs", [&] { uv::AlignSelected(m_Mesh, faces, 0); });
                if (ImGui::MenuItem("Horizontally (same V)")) UVOperate("Align UVs", [&] { uv::AlignSelected(m_Mesh, faces, 1); });
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Mirror", sel))
            {
                if (ImGui::MenuItem("Left to Right (U)")) UVOperate("Mirror UVs", [&] { uv::MirrorSelected(m_Mesh, faces, 0); });
                if (ImGui::MenuItem("Top to Bottom (V)")) UVOperate("Mirror UVs", [&] { uv::MirrorSelected(m_Mesh, faces, 1); });
                ImGui::EndMenu();
            }
            auto rotate = [&](float degrees) {
                glm::vec2 c;
                if (!uv::SelectionCenter(m_Mesh, faces, c)) return;
                float a = glm::radians(degrees), cs = std::cos(a), sn = std::sin(a);
                glm::mat3 t(1.0f), r(cs, sn, 0, -sn, cs, 0, 0, 0, 1), back(1.0f);
                t[2] = glm::vec3(-c, 1.0f);
                back[2] = glm::vec3(c, 1.0f);
                uv::TransformSelected(m_Mesh, faces, back * r * t);
            };
            if (ImGui::MenuItem("Rotate 90 Clockwise", nullptr, false, sel)) UVOperate("Rotate UVs", [&] { rotate(90.0f); });
            if (ImGui::MenuItem("Rotate 90 Counter-Clockwise", nullptr, false, sel)) UVOperate("Rotate UVs", [&] { rotate(-90.0f); });
        }
        else
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Open UV Editor")) m_Ctx.OpenUVEditor = true;
        }
    }

    // ---- The UV Editor's view

    ImVec2 MeshEditMode::UVToScreen(const glm::vec2& p) const
    {
        return ImVec2(m_UVCanvasMin.x + m_UVPan.x + p.x * m_UVZoom, m_UVCanvasMin.y + m_UVPan.y + p.y * m_UVZoom);
    }

    glm::ivec2 MeshEditMode::UVTextureSize() const
    {
        AssetHandle bg = UVBackground();
        Ref<Texture2D> t = bg.IsValid() ? AssetManager::GetTexture(bg) : nullptr;
        if (!t || t->GetWidth() == 0 || t->GetHeight() == 0) return { 64, 64 };
        return { (int)t->GetWidth(), (int)t->GetHeight() };
    }

    std::vector<uint32_t> MeshEditMode::UVFaces() const { return m_UVSync ? uv::VisibleFaces(m_Mesh) : uv::SelectedFaces(m_Mesh); }

    glm::vec2 MeshEditMode::ScreenToUV(const ImVec2& p) const
    {
        return glm::vec2((p.x - m_UVCanvasMin.x - m_UVPan.x) / m_UVZoom, (p.y - m_UVCanvasMin.y - m_UVPan.y) / m_UVZoom);
    }

    void MeshEditMode::FrameUVs(bool selectedOnly)
    {
        glm::vec2 mn(0.0f), mx(1.0f);
        if (selectedOnly && m_Active)
        {
            std::vector<uint32_t> faces = uv::SelectedFaces(m_Mesh);
            glm::vec2 a(1e30f), b(-1e30f);
            bool any = false;
            for (const uv::Corner& c : uv::SelectedCorners(m_Mesh, faces))
            {
                glm::vec2 p = CornerUV(m_Mesh.Faces[c.Face], c.Index);
                a = glm::min(a, p);
                b = glm::max(b, p);
                any = true;
            }
            if (any)
            {
                mn = a;
                mx = b;
            }
        }
        glm::vec2 ext = glm::max(mx - mn, glm::vec2(0.02f));
        glm::vec2 center = (mn + mx) * 0.5f;
        m_UVZoom = std::max(std::min(m_UVCanvasSize.x, m_UVCanvasSize.y) * 0.9f / std::max(ext.x, ext.y), 1.0f);
        m_UVPan = ImVec2(m_UVCanvasSize.x * 0.5f - center.x * m_UVZoom, m_UVCanvasSize.y * 0.5f - center.y * m_UVZoom);
        m_UVFramed = true;
    }

    AssetHandle MeshEditMode::UVBackground() const
    {
        if (!m_Active) return AssetHandle::None();
        Scene& scene = m_Ctx.ActiveScene();
        if (!scene.IsValid(m_Entity) || !scene.GetRegistry().Has<MeshRendererComponent>(m_Entity)) return AssetHandle::None();
        const auto& mr = scene.GetRegistry().Get<MeshRendererComponent>(m_Entity);
        AssetHandle material = !mr.Materials.empty() && mr.Materials[0].IsValid() ? mr.Materials[0] : mr.Material;
        if (material.IsValid())
            if (Ref<Material> mat = AssetManager::GetMaterial(material); mat && mat->BaseColorMap.IsValid()) return mat->BaseColorMap;
        return mr.Texture;
    }

    // ---- Selection in the UV Editor

    void MeshEditMode::UVClickSelect(const std::vector<uint32_t>& faces, const ImVec2& mouse, bool extend)
    {
        Record("UV Select");
        if (!extend) uv::SelectAll(m_Mesh, faces, false);
        std::vector<uint32_t> tri;
        auto faceUnder = [&]() -> int64_t {
            for (uint32_t f : faces)
            {
                const Face& face = m_Mesh.Faces[f];
                tri.clear();
                m_Mesh.TriangulateFace(f, tri);
                for (size_t k = 0; k + 2 < tri.size(); k += 3)
                    if (InTriangle(mouse, UVToScreen(CornerUV(face, tri[k])), UVToScreen(CornerUV(face, tri[k + 1])), UVToScreen(CornerUV(face, tri[k + 2]))))
                        return f;
            }
            return -1;
        };
        if (m_UVMode == 0)
        {
            float best = 12.0f;
            uv::Corner hit{};
            bool found = false;
            for (uint32_t f : faces)
                for (uint32_t c = 0; c < m_Mesh.Faces[f].Verts.size(); ++c)
                    if (float d = Dist(UVToScreen(CornerUV(m_Mesh.Faces[f], c)), mouse); d < best)
                    {
                        best = d;
                        hit = { f, c };
                        found = true;
                    }
            if (found) uv::SelectCorner(m_Mesh, faces, hit, extend ? !uv::IsSelected(m_Mesh, hit) : true, m_UVSticky);
        }
        else if (m_UVMode == 1)
        {
            float best = 10.0f;
            uv::Corner hit{};
            bool found = false;
            for (uint32_t f : faces)
            {
                const Face& face = m_Mesh.Faces[f];
                uint32_t n = (uint32_t)face.Verts.size();
                for (uint32_t c = 0; c < n; ++c)
                    if (float d = SegmentDist(mouse, UVToScreen(CornerUV(face, c)), UVToScreen(CornerUV(face, (c + 1) % n))); d < best)
                    {
                        best = d;
                        hit = { f, c };
                        found = true;
                    }
            }
            if (found)
            {
                uint32_t n = (uint32_t)m_Mesh.Faces[hit.Face].Verts.size();
                uv::Corner other{ hit.Face, (hit.Index + 1) % n };
                bool on = extend ? !(uv::IsSelected(m_Mesh, hit) && uv::IsSelected(m_Mesh, other)) : true;
                uv::SelectCorner(m_Mesh, faces, hit, on, m_UVSticky);
                uv::SelectCorner(m_Mesh, faces, other, on, m_UVSticky);
            }
        }
        else if (int64_t f = faceUnder(); f >= 0)
        {
            const Face& face = m_Mesh.Faces[(size_t)f];
            bool all = true;
            for (uint32_t c = 0; c < face.Verts.size(); ++c) all &= uv::IsSelected(m_Mesh, { (uint32_t)f, c });
            bool on = extend ? !all : true;
            if (m_UVMode == 3)
            {
                uv::Corner seed{ (uint32_t)f, 0 };
                uv::SelectLinked(m_Mesh, faces, &seed, on);
            }
            else
                for (uint32_t c = 0; c < face.Verts.size(); ++c) uv::SelectCorner(m_Mesh, faces, { (uint32_t)f, c }, on, m_UVSticky);
        }
        SelectionChanged();
    }

    void MeshEditMode::UVBoxSelect(const std::vector<uint32_t>& faces, const ImVec2& a, const ImVec2& b, int op)
    {
        Record("UV Box Select");
        if (op == 0) uv::SelectAll(m_Mesh, faces, false);
        ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
        auto inside = [&](ImVec2 p) { return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y; };
        bool on = op != 2;
        for (uint32_t f : faces)
        {
            Face& face = m_Mesh.Faces[f];
            if (m_UVMode <= 1)
            {
                for (size_t c = 0; c < face.Verts.size(); ++c)
                    if (inside(UVToScreen(CornerUV(face, c)))) face.SetCornerFlag(c, UVSelected, on);
                continue;
            }
            glm::vec2 center(0.0f);
            for (size_t c = 0; c < face.Verts.size(); ++c) center += CornerUV(face, c);
            center /= (float)std::max<size_t>(face.Verts.size(), 1);
            if (!inside(UVToScreen(center))) continue;
            if (m_UVMode == 3 && !on)
            {
                uv::Corner seed{ f, 0 };
                uv::SelectLinked(m_Mesh, faces, &seed, false);
            }
            else
                for (size_t c = 0; c < face.Verts.size(); ++c) face.SetCornerFlag(c, UVSelected, on);
        }
        if (on)
        {
            if (m_UVMode == 3) uv::SelectLinked(m_Mesh, faces);
            else uv::ExpandSticky(m_Mesh, faces, m_UVSticky);
        }
        SelectionChanged();
    }

    // ---- Moving UVs (G, R, S)

    void MeshEditMode::BeginUVModal(UVModalKind kind)
    {
        std::vector<uint32_t> faces = UVFaces();
        std::vector<uv::Corner> sel = uv::SelectedCorners(m_Mesh, faces);
        if (sel.empty()) return;
        static const char* names[] = { "", "Move UVs", "Rotate UVs", "Scale UVs" };
        Record(names[(int)kind]);
        m_UVModal = {};
        m_UVModal.Kind = kind;
        for (const uv::Corner& c : sel) m_UVModal.Start.push_back({ c, CornerUV(m_Mesh.Faces[c.Face], c.Index) });
        uv::SelectionCenter(m_Mesh, faces, m_UVModal.Pivot);
        m_UVModal.MouseStart = ImGui::GetMousePos();
        ImVec2 ps = UVToScreen(m_UVModal.Pivot);
        m_UVModal.LastAngle = std::atan2(m_UVModal.MouseStart.y - ps.y, m_UVModal.MouseStart.x - ps.x);
        m_UVModal.StartFrame = ImGui::GetFrameCount();
    }

    void MeshEditMode::UpdateUVModal(const std::vector<uint32_t>&)
    {
        UVModal& md = m_UVModal;
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = ImGui::GetMousePos();
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) md.Axis = md.Axis == 0 ? -1 : 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) md.Axis = md.Axis == 1 ? -1 : 1;
        ImVec2 ps = UVToScreen(md.Pivot);
        glm::mat3 m(1.0f);
        glm::mat3 toPivot(1.0f), fromPivot(1.0f);
        toPivot[2] = glm::vec3(-md.Pivot, 1.0f);
        fromPivot[2] = glm::vec3(md.Pivot, 1.0f);
        if (md.Kind == UVModalKind::Move)
        {
            glm::vec2 d((mouse.x - md.MouseStart.x) / m_UVZoom, (mouse.y - md.MouseStart.y) / m_UVZoom);
            if (io.KeyCtrl) d = glm::round(d * glm::vec2(UVTextureSize())) / glm::vec2(UVTextureSize()); // whole pixels
            if (md.Axis == 0) d.y = 0.0f;
            if (md.Axis == 1) d.x = 0.0f;
            m[2] = glm::vec3(d, 1.0f);
        }
        else if (md.Kind == UVModalKind::Rotate)
        {
            float a = std::atan2(mouse.y - ps.y, mouse.x - ps.x), delta = a - md.LastAngle;
            while (delta > glm::pi<float>()) delta -= glm::two_pi<float>();
            while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
            md.Angle += delta;
            md.LastAngle = a;
            float angle = io.KeyCtrl ? glm::radians(std::round(glm::degrees(md.Angle) / 5.0f) * 5.0f) : md.Angle;
            float cs = std::cos(angle), sn = std::sin(angle);
            m = fromPivot * glm::mat3(cs, sn, 0, -sn, cs, 0, 0, 0, 1) * toPivot;
        }
        else
        {
            float s = Dist(mouse, ps) / std::max(Dist(md.MouseStart, ps), 1.0f);
            if (io.KeyCtrl) s = std::round(s * 10.0f) / 10.0f;
            glm::vec2 k(md.Axis == 1 ? 1.0f : s, md.Axis == 0 ? 1.0f : s);
            m = fromPivot * glm::mat3(k.x, 0, 0, 0, k.y, 0, 0, 0, 1) * toPivot;
        }
        for (auto& [c, start] : md.Start)
        {
            Face& face = m_Mesh.Faces[c.Face];
            if (face.UVs.size() != face.Verts.size()) face.UVs.resize(face.Verts.size(), glm::vec2(0.0f));
            face.UVs[c.Index] = glm::vec2(m * glm::vec3(start, 1.0f));
            if (m_UVPixelSnap)
            {
                glm::vec2 px(UVTextureSize());
                face.UVs[c.Index] = glm::round(face.UVs[c.Index] * px) / px;
            }
        }
        Commit();
        bool later = ImGui::GetFrameCount() > md.StartFrame;
        if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            EndUVModal(true);
        else if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
            EndUVModal(false);
    }

    void MeshEditMode::EndUVModal(bool confirm)
    {
        if (m_UVModal.Kind == UVModalKind::None) return;
        if (!confirm)
        {
            for (auto& [c, start] : m_UVModal.Start)
                if (c.Face < m_Mesh.FaceCount() && c.Index < m_Mesh.Faces[c.Face].UVs.size()) m_Mesh.Faces[c.Face].UVs[c.Index] = start;
            if (!m_Undo.empty()) m_Undo.pop_back();
            Commit();
        }
        m_UVModal = {};
    }

    // ---- Keys and mouse

    void MeshEditMode::HandleUVKeys(const std::vector<uint32_t>& faces, bool hovered)
    {
        using Shortcuts::Pressed;
        auto selectOp = [&](const char* name, auto fn) {
            Record(name);
            fn();
            SelectionChanged();
        };
        if (Pressed("uv.vertexMode")) m_UVMode = 0;
        if (Pressed("uv.edgeMode")) m_UVMode = 1;
        if (Pressed("uv.faceMode")) m_UVMode = 2;
        if (Pressed("uv.islandMode")) m_UVMode = 3;
        if (Pressed("uv.selectAll")) selectOp("Select All UVs", [&] { uv::SelectAll(m_Mesh, faces); });
        if (Pressed("uv.deselectAll")) selectOp("Select No UVs", [&] { uv::SelectAll(m_Mesh, faces, false); });
        if (Pressed("uv.invert")) selectOp("Invert UV Selection", [&] { uv::InvertSelection(m_Mesh, faces); });
        if (Pressed("uv.selectLinked")) selectOp("Select Linked UVs", [&] { uv::SelectLinked(m_Mesh, faces); });
        if (Pressed("uv.linkedPick") && hovered)
        {
            // The island under the mouse (its nearest corner).
            ImVec2 mouse = ImGui::GetMousePos();
            float best = 1e30f;
            uv::Corner hit{};
            bool found = false;
            for (uint32_t f : faces)
                for (uint32_t c = 0; c < m_Mesh.Faces[f].Verts.size(); ++c)
                    if (float d = Dist(UVToScreen(CornerUV(m_Mesh.Faces[f], c)), mouse); d < best)
                    {
                        best = d;
                        hit = { f, c };
                        found = true;
                    }
            if (found) selectOp("Select Linked UVs", [&] { uv::SelectLinked(m_Mesh, faces, &hit, !ImGui::GetIO().KeyShift); });
        }
        if (Pressed("uv.grab")) BeginUVModal(UVModalKind::Move);
        if (Pressed("uv.stitch")) UVOperate("Stitch UVs", [&] { Log::Info("Stitched {} vertices", uv::Stitch(m_Mesh, faces, m_StitchSnap)); });
        if (Pressed("uv.rotate")) BeginUVModal(UVModalKind::Rotate);
        if (Pressed("uv.scale")) BeginUVModal(UVModalKind::Scale);
        if (Pressed("uv.pin")) UVOperate("Pin", [&] { uv::SetPinned(m_Mesh, faces, true); });
        if (Pressed("uv.unpin")) UVOperate("Unpin", [&] { uv::SetPinned(m_Mesh, faces, false); });
        if (Pressed("uv.unwrapMenu")) m_UVOpenMenu = 1;
        if (Pressed("uv.merge")) m_UVOpenMenu = 2;
        if (Pressed("uv.frame")) FrameUVs(true);
        if (Pressed("uv.frameAll")) FrameUVs(false);
    }

    void MeshEditMode::UpdateUVPaint(bool hovered)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = ImGui::GetMousePos();
        glm::vec2 size((float)m_PaintImage.Width, (float)m_PaintImage.Height);
        float radius = m_PaintRadiusPx / std::max(m_UVZoom, 1e-3f) * size.x; // (in the texture's pixels)
        auto dab = [&](ImVec2 at) {
            pt::Brush b = PaintBrush(glm::vec3(0.0f), glm::vec3(0, 0, -1), io.KeyShift);
            b.Texture = b.Texture && m_PaintTex.Mapping != 2 ? b.Texture : nullptr; // (a stencil belongs to the 3D view)
            pt::Rect r = pt::PaintImage(m_PaintImage, ScreenToUV(at) * size, radius, b);
            if (r.Empty()) return;
            m_PaintStrokeRect.Add(r);
            m_PaintGpuDirty = m_PaintUnsaved = true;
        };
        if (!m_UVPainting)
        {
            if (!hovered || io.KeyAlt || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
            if (m_PaintBlend == pt::Blend::Clone && !io.KeyShift)
            {
                if (io.KeyCtrl)
                {
                    m_CloneAnchorUV = ScreenToUV(mouse);
                    m_CloneAnchorSet = true;
                    return;
                }
                if (!BeginClone(ScreenToUV(mouse))) return;
            }
            Record("Texture Paint");
            BeginTextureStroke();
            m_UVPainting = true;
            m_UVPaintMouse = mouse;
            dab(mouse);
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_UVPainting = false;
            EndTextureStroke();
            return;
        }
        float spacing = std::max(2.0f, m_PaintRadiusPx * 0.2f);
        int steps = std::min((int)(Dist(mouse, m_UVPaintMouse) / spacing), 16);
        for (int i = 1; i <= steps; ++i)
        {
            float k = (float)i / (float)steps;
            dab(ImVec2(m_UVPaintMouse.x + (mouse.x - m_UVPaintMouse.x) * k, m_UVPaintMouse.y + (mouse.y - m_UVPaintMouse.y) * k));
        }
        if (steps > 0) m_UVPaintMouse = mouse;
    }

    void MeshEditMode::HandleUVMouse(const std::vector<uint32_t>& faces, bool hovered, bool selectWithLeft)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = ImGui::GetMousePos();
        if (hovered && io.MouseWheel != 0.0f)
        {
            glm::vec2 at = ScreenToUV(mouse);
            m_UVZoom = std::clamp(m_UVZoom * std::pow(1.15f, io.MouseWheel), 10.0f, 200000.0f);
            m_UVPan = ImVec2(mouse.x - m_UVCanvasMin.x - at.x * m_UVZoom, mouse.y - m_UVCanvasMin.y - at.y * m_UVZoom);
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_UVPanning = true;
        if (m_UVPanning)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            {
                m_UVPan.x += io.MouseDelta.x;
                m_UVPan.y += io.MouseDelta.y;
            }
            else
                m_UVPanning = false;
        }
        if (selectWithLeft && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_UVPressing = true;
            m_UVBoxSelecting = false;
            m_UVPressPos = mouse;
        }
        if (m_UVPressing)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (Dist(mouse, m_UVPressPos) > 4.0f) m_UVBoxSelecting = true;
            }
            else
            {
                if (m_UVBoxSelecting) UVBoxSelect(faces, m_UVPressPos, mouse, io.KeyShift ? 1 : io.KeyCtrl ? 2 : 0);
                else UVClickSelect(faces, m_UVPressPos, io.KeyShift);
                m_UVPressing = m_UVBoxSelecting = false;
            }
        }
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0.0f) m_UVOpenMenu = 1;
    }

    // ---- The window

    void MeshEditMode::DrawUVEditor(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(620, 660), ImGuiCond_FirstUseEver);
        bool visible = ImGui::Begin("UV Editor", open, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (!visible || !m_Active)
        {
            if (visible)
            {
                if (ImGui::BeginMenuBar()) ImGui::EndMenuBar();
                ImGui::TextDisabled("Edit a model (select it and press %s in the Scene view) to see its UVs here.", Shortcuts::Label("mesh.editMode").c_str());
            }
            if (m_UVModal.Kind != UVModalKind::None) EndUVModal(false);
            ImGui::End();
            return;
        }
        std::vector<uint32_t> faces = UVFaces();
        // With sync selection the mesh's selection is the truth: read it, and write back what the editor changes.
        if (m_UVSync && m_UVModal.Kind == UVModalKind::None) uv::SyncSelectionFromMesh(m_Mesh, faces);
        auto uvSelectionHash = [&] {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t f : faces)
                for (size_t c = 0; c < m_Mesh.Faces[f].Verts.size(); ++c) h = (h ^ (m_Mesh.Faces[f].CornerFlags(c) & UVSelected)) * 1099511628211ull;
            return h;
        };
        uint64_t selectionBefore = m_UVSync ? uvSelectionHash() : 0;

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Frame All", Shortcuts::Label("uv.frameAll").c_str())) FrameUVs(false);
                if (ImGui::MenuItem("Frame Selected", Shortcuts::Label("uv.frame").c_str())) FrameUVs(true);
                ImGui::Separator();
                ImGui::MenuItem("Show Texture", nullptr, &m_UVShowTexture);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The object's base color texture (or a checker when it has none) under the UVs");
                if (ImGui::MenuItem("Stretch: Off", nullptr, m_UVStretch < 0)) m_UVStretch = -1;
                if (ImGui::MenuItem("Stretch: Angle", nullptr, m_UVStretch == 0)) m_UVStretch = 0;
                if (ImGui::MenuItem("Stretch: Area", nullptr, m_UVStretch == 1)) m_UVStretch = 1;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Select"))
            {
                auto act = [&](const char* name, const char* shortcut, auto fn) {
                    if (ImGui::MenuItem(name, shortcut ? Shortcuts::Label(shortcut).c_str() : nullptr))
                    {
                        Record(name);
                        fn();
                        SelectionChanged();
                    }
                };
                act("All", "uv.selectAll", [&] { uv::SelectAll(m_Mesh, faces); });
                act("None", "uv.deselectAll", [&] { uv::SelectAll(m_Mesh, faces, false); });
                act("Invert", "uv.invert", [&] { uv::InvertSelection(m_Mesh, faces); });
                act("Linked", "uv.selectLinked", [&] { uv::SelectLinked(m_Mesh, faces); });
                act("Split", nullptr, [&] { uv::SplitSelection(m_Mesh, faces); });
                ImGui::Separator();
                ImGui::TextDisabled("Click: select   Shift+click: toggle   Drag: box (Shift add, Ctrl remove)");
                ImGui::TextDisabled("L: island under the mouse   Middle drag: pan   Wheel: zoom");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("UV"))
            {
                UVMenuItems(true);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Toolbar: select mode, sticky selection, stretch.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
        const char* modeKeys[] = { "uv.vertexMode", "uv.edgeMode", "uv.faceMode", "uv.islandMode" };
        for (int i = 0; i < 4; ++i)
        {
            if (i) ImGui::SameLine(0.0f, 2.0f);
            bool on = m_UVMode == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kUVModeNames[i])) m_UVMode = i;
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s select (%s)", kUVModeNames[i], Shortcuts::Label(modeKeys[i]).c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        const char* stickies[] = { "Shared Location", "Disabled", "Shared Vertex" };
        int sticky = (int)m_UVSticky;
        if (ImGui::Combo("##sticky", &sticky, stickies, 3)) m_UVSticky = (uv::Sticky)sticky;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sticky selection: which other face corners a clicked UV brings along (the same vertex at the same place, none, or "
                              "every corner of the vertex)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const char* stretches[] = { "No Stretch", "Angle Stretch", "Area Stretch" };
        int stretch = m_UVStretch + 1;
        if (ImGui::Combo("##stretch", &stretch, stretches, 3)) m_UVStretch = stretch - 1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colors faces by how distorted their UVs are: blue true to the surface, red badly stretched");
        // (the rest wraps onto another line when the window is narrow)
        auto next = [](const char* label) {
            ImGui::SameLine();
            float w = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
            if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
        };
        next("Texture");
        ImGui::Checkbox("Texture", &m_UVShowTexture);
        next("Sync");
        bool syncKey = Shortcuts::Pressed("uv.sync");
        if (syncKey) m_UVSync = !m_UVSync;
        if (ImGui::Checkbox("Sync", &m_UVSync) || syncKey)
        {
            if (m_UVSync)
            {
                faces = UVFaces();
                uv::SyncSelectionFromMesh(m_Mesh, faces);
                selectionBefore = uvSelectionHash();
            }
            else faces = UVFaces();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("UV Sync Selection: shows every face, and selecting UVs selects the mesh's vertices (and the other way round)");
        next("Pixel Snap");
        ImGui::Checkbox("Pixel Snap", &m_UVPixelSnap);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moved, turned and scaled UVs land on the texture's pixel corners");
        bool canPaint = m_PaintMode == 2 && !m_PaintImage.Empty();
        if (Shortcuts::Pressed("uv.paint")) m_UVPaint = !m_UVPaint;
        if (canPaint)
        {
            next("Paint");
            ImGui::Checkbox("Paint", &m_UVPaint);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Paint the texture right here with the Texture Paint brush (left drag; Shift blurs; the middle button pans)");
        }
        bool painting2D = canPaint && m_UVPaint;
        ImGui::PopStyleVar();

        // The canvas.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        avail.x = std::max(avail.x, 50.0f);
        avail.y = std::max(avail.y, 50.0f);
        m_UVCanvasMin = ImGui::GetCursorScreenPos();
        m_UVCanvasSize = avail;
        if (!m_UVFramed) FrameUVs(false);
        ImGui::InvisibleButton("##uvcanvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cmax(m_UVCanvasMin.x + avail.x, m_UVCanvasMin.y + avail.y);
        dl->PushClipRect(m_UVCanvasMin, cmax, true);
        dl->AddRectFilled(m_UVCanvasMin, cmax, IM_COL32(38, 38, 40, 255));
        ImVec2 t0 = UVToScreen({ 0, 0 }), t1 = UVToScreen({ 1, 1 });
        Ref<Texture2D> texture;
        if (m_UVShowTexture)
        {
            AssetHandle bg = UVBackground();
            texture = bg.IsValid() ? AssetManager::GetTexture(bg) : nullptr;
            if (!texture) texture = AssetManager::GetCheckerTexture();
        }
        if (texture) dl->AddImage(ImTextureRef((ImTextureID)texture->GetImGuiId()), t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 170));
        else dl->AddRectFilled(t0, t1, IM_COL32(58, 58, 62, 255));
        for (int i = 1; i < 10; ++i)
        {
            float f = (float)i / 10.0f;
            ImU32 c = IM_COL32(255, 255, 255, i == 5 ? 45 : 22);
            dl->AddLine(UVToScreen({ f, 0 }), UVToScreen({ f, 1 }), c);
            dl->AddLine(UVToScreen({ 0, f }), UVToScreen({ 1, f }), c);
        }
        dl->AddRect(t0, t1, IM_COL32(255, 255, 255, 110));

        // Faces, edges and corners.
        std::vector<float> stretch01;
        if (m_UVStretch >= 0) stretch01 = uv::FaceStretch(m_Mesh, faces, (uv::StretchMode)m_UVStretch);
        std::vector<uint32_t> tri;
        std::vector<ImVec2> pts;
        for (size_t i = 0; i < faces.size(); ++i)
        {
            const Face& face = m_Mesh.Faces[faces[i]];
            size_t n = face.Verts.size();
            bool all = true;
            for (size_t c = 0; c < n; ++c) all &= (face.CornerFlags(c) & UVSelected) != 0;
            ImU32 fill = !stretch01.empty() ? StretchColor(stretch01[i]) : all ? kUVFaceSelected : 0;
            if (!fill) continue;
            tri.clear();
            m_Mesh.TriangulateFace(faces[i], tri);
            for (size_t k = 0; k + 2 < tri.size(); k += 3)
                dl->AddTriangleFilled(UVToScreen(CornerUV(face, tri[k])), UVToScreen(CornerUV(face, tri[k + 1])), UVToScreen(CornerUV(face, tri[k + 2])), fill);
        }
        for (uint32_t f : faces)
        {
            const Face& face = m_Mesh.Faces[f];
            size_t n = face.Verts.size();
            for (size_t c = 0; c < n; ++c)
            {
                size_t d = (c + 1) % n;
                bool sel = (face.CornerFlags(c) & UVSelected) && (face.CornerFlags(d) & UVSelected);
                bool seam = m_Mesh.GetEdge(MakeEdgeKey(face.Verts[c], face.Verts[d])).Seam;
                dl->AddLine(UVToScreen(CornerUV(face, c)), UVToScreen(CornerUV(face, d)), sel ? kUVSelected : seam ? kUVSeam : kUVWire, sel || seam ? 2.0f : 1.0f);
            }
        }
        for (uint32_t f : faces)
        {
            const Face& face = m_Mesh.Faces[f];
            for (size_t c = 0; c < face.Verts.size(); ++c)
            {
                ImVec2 p = UVToScreen(CornerUV(face, c));
                uint8_t bits = face.CornerFlags(c);
                if (bits & UVPinned) dl->AddRectFilled(ImVec2(p.x - 3.5f, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y + 3.5f), kUVPin);
                else if (m_UVMode == 0) dl->AddCircleFilled(p, (bits & UVSelected) ? 3.5f : 2.5f, (bits & UVSelected) ? kUVSelected : kUVVertex);
            }
            if (m_UVMode >= 2)
            {
                glm::vec2 center(0.0f);
                bool all = true;
                for (size_t c = 0; c < face.Verts.size(); ++c)
                {
                    center += CornerUV(face, c);
                    all &= (face.CornerFlags(c) & UVSelected) != 0;
                }
                center /= (float)std::max<size_t>(face.Verts.size(), 1);
                dl->AddCircleFilled(UVToScreen(center), 2.5f, all ? kUVSelected : kUVVertex);
            }
        }

        // Interaction.
        if (m_UVModal.Kind != UVModalKind::None) UpdateUVModal(faces);
        else if (painting2D)
        {
            HandleUVMouse(faces, hovered, false);
            UpdateUVPaint(hovered);
            if (m_PaintGpuDirty)
            {
                AssetManager::SetTexturePixels(m_PaintTexture, m_PaintImage.Pixels.data(), m_PaintImage.Width, m_PaintImage.Height);
                m_PaintGpuDirty = false;
            }
            if (hovered || m_UVPainting)
            {
                glm::vec4 pc = m_PaintColor;
                ImVec2 mp = ImGui::GetMousePos();
                dl->AddCircle(mp, m_PaintRadiusPx, IM_COL32(0, 0, 0, 160), 64, 3.0f);
                dl->AddCircle(mp, m_PaintRadiusPx, IM_COL32((int)(pc.r * 255), (int)(pc.g * 255), (int)(pc.b * 255), 230), 64, 1.5f);
            }
        }
        else
        {
            if (m_UVPainting)
            {
                m_UVPainting = false;
                EndTextureStroke();
            }
            if ((hovered || ImGui::IsWindowFocused()) && !ImGui::GetIO().WantTextInput) HandleUVKeys(faces, hovered);
            HandleUVMouse(faces, hovered);
        }
        if (m_UVBoxSelecting && m_UVPressing)
        {
            ImVec2 a = m_UVPressPos, b = ImGui::GetMousePos();
            ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
            dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 25));
            dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 180));
        }

        // Status: what's shown, or the transform under way.
        std::string status;
        char buf[200];
        if (m_UVSync && m_UVModal.Kind == UVModalKind::None && uvSelectionHash() != selectionBefore)
        {
            uv::SyncSelectionToMesh(m_Mesh, faces);
            SelectionChanged();
        }
        if (faces.empty()) status = "Select faces in the Scene view to see their UVs";
        else
        {
            std::snprintf(buf, sizeof(buf), "%zu faces   %zu UVs selected", faces.size(), uv::SelectedCorners(m_Mesh, faces).size());
            status = buf;
        }
        if (m_UVModal.Kind != UVModalKind::None)
        {
            static const char* names[] = { "", "Move", "Rotate", "Scale" };
            status = names[(int)m_UVModal.Kind];
            if (m_UVModal.Axis >= 0) status += m_UVModal.Axis == 0 ? "  along U" : "  along V";
            if (m_UVModal.Kind == UVModalKind::Rotate)
            {
                std::snprintf(buf, sizeof(buf), "  %.1f deg", glm::degrees(m_UVModal.Angle));
                status += buf;
            }
            status += "   X/Y axis, Ctrl snap (whole pixels when moving); click or Enter: done, right-click or Esc: cancel";
        }
        dl->AddText(ImVec2(m_UVCanvasMin.x + 8.0f, cmax.y - 22.0f), IM_COL32(255, 255, 255, 220), status.c_str());
        dl->PopClipRect();

        // Menus at the mouse.
        if (m_UVOpenMenu)
        {
            ImGui::OpenPopup(m_UVOpenMenu == 2 ? "UVMergeMenu" : "UVEditorMenu");
            m_UVOpenMenu = 0;
        }
        if (ImGui::BeginPopup("UVEditorMenu"))
        {
            ImGui::TextDisabled("UV");
            UVMenuItems(true);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("UVMergeMenu"))
        {
            ImGui::TextDisabled("Merge UVs");
            if (ImGui::MenuItem("At Center")) UVOperate("Merge UVs", [&] { uv::WeldSelected(m_Mesh, faces); });
            if (ImGui::MenuItem("By Distance"))
                UVOperate("Merge UVs by Distance", [&] { Log::Info("Merged {} UVs", uv::MergeByDistance(m_Mesh, faces, m_UVMergeDistance)); });
            ImGui::SetNextItemWidth(100.0f);
            ImGui::DragFloat("Distance", &m_UVMergeDistance, 0.0001f, 0.0f, 0.5f, "%.4f");
            ImGui::EndPopup();
        }
        ImGui::End();
    }
}
