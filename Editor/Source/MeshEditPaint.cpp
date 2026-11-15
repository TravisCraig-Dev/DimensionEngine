#include "MeshEditMode.h"
#include "EditorUI.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Scene/Components.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Edit Mode's Vertex Paint and Texture Paint, and baking maps.
namespace Editor
{
    using namespace Dimension::Modeling;
    namespace pt = Dimension::Modeling::Paint;

    namespace
    {
        float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
        constexpr int kTile = 64;
    }

    // ---- Modes

    void MeshEditMode::SetPaintMode(int mode)
    {
        if (!m_Active || mode == m_PaintMode) return;
        if (m_Modal.Kind != ModalKind::None) EndModal(false);
        m_Tool = Tool::None;
        if (m_Sculpting) SetSculpting(false);
        if (mode && m_RigMode) SetRigMode(0);
        if (mode && m_CurveMode) SetCurveMode(false);
        if (m_PaintMode == 2) SavePaintTexture();
        m_PaintMode = std::clamp(mode, 0, 2);
        m_Painting = false;
        m_PaintAdjust = 0;
        m_PaintRaysVersion = ~0ull;
        m_Pressing = m_BoxSelecting = false;
        if (m_PaintMode == 2 && !LoadPaintTexture())
            Log::Warn("Texture Paint: this object has no PNG base color texture to paint on yet. Use New Texture in the toolbar.");
        if (m_PaintMode == 0) ++m_GeometryVersion;
    }

    AssetHandle MeshEditMode::PaintTarget() const
    {
        Scene& scene = m_Ctx.ActiveScene();
        if (!m_Active || !scene.IsValid(m_Entity) || !scene.GetRegistry().Has<MeshRendererComponent>(m_Entity)) return AssetHandle::None();
        const auto& mr = scene.GetRegistry().Get<MeshRendererComponent>(m_Entity);
        AssetHandle material = !mr.Materials.empty() && mr.Materials[0].IsValid() ? mr.Materials[0] : mr.Material;
        if (material.IsValid())
            if (Ref<Material> mat = AssetManager::GetMaterial(material)) return mat->BaseColorMap;
        return mr.Texture;
    }

    void MeshEditMode::AssignBaseColorTexture(AssetHandle texture)
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto& mr = scene.GetRegistry().Get<MeshRendererComponent>(m_Entity);
        AssetHandle material = !mr.Materials.empty() && mr.Materials[0].IsValid() ? mr.Materials[0] : mr.Material;
        if (material.IsValid() && !AssetManager::IsRuntimeMaterial(material))
            if (Ref<Material> mat = AssetManager::GetMaterial(material))
            {
                mat->BaseColorMap = texture;
                mat->Save(AssetManager::GetAbsolutePath(material));
                return;
            }
        mr.Texture = texture;
        m_Ctx.MarkDirty();
    }

    bool MeshEditMode::LoadPaintTexture()
    {
        AssetHandle target = PaintTarget();
        if (target == m_PaintTexture && !m_PaintImage.Empty()) return true;
        if (m_PaintUnsaved) SavePaintTexture();
        m_PaintTexture = target;
        m_PaintImage = {};
        if (!target.IsValid()) return false;
        const AssetMetadata* meta = AssetManager::GetMetadata(target);
        if (!meta || meta->Type != AssetType::Texture) return false;
        std::string ext = meta->Path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
        if (ext != ".png")
        {
            Log::Warn("Texture Paint paints PNG textures; '{}' is not one (New Texture makes one)", meta->Path.filename().string());
            return false;
        }
        if (!pt::Image::Load(AssetManager::GetAbsolutePath(target), m_PaintImage))
        {
            Log::Error("Could not read {}", meta->Path.string());
            return false;
        }
        return true;
    }

    void MeshEditMode::SavePaintTexture()
    {
        if (!m_PaintUnsaved || m_PaintImage.Empty() || !m_PaintTexture.IsValid()) return;
        fs::path file = AssetManager::GetAbsolutePath(m_PaintTexture);
        if (m_PaintImage.SavePng(file))
        {
            m_PaintUnsaved = false;
            Log::Info("Saved the painted texture {}", file.filename().string());
        }
        else
            Log::Error("Could not write {}", file.string());
    }

    void MeshEditMode::NewPaintTexture()
    {
        fs::path folder = AssetManager::GetAssetsDirectory() / "Textures";
        std::error_code ec;
        fs::create_directories(folder, ec);
        std::string stem = m_File.stem().string() + "_Color";
        fs::path path = folder / (stem + ".png");
        for (int i = 1; fs::exists(path); ++i) path = folder / (stem + " " + std::to_string(i) + ".png");
        pt::Image image = pt::Image::Solid((uint32_t)m_NewTextureSize, (uint32_t)m_NewTextureSize, PackRGBA8(m_NewTextureColor));
        if (!image.SavePng(path))
        {
            Log::Error("Could not write {}", path.string());
            return;
        }
        AssetHandle handle = AssetManager::Import(path);
        if (!handle.IsValid()) return;
        if (m_PaintUnsaved) SavePaintTexture();
        AssignBaseColorTexture(handle);
        m_PaintTexture = AssetHandle::None();
        LoadPaintTexture();
        Log::Info("New texture {} for painting", path.filename().string());
    }

    void MeshEditMode::SwapPaintUndo(PaintUndo& undo)
    {
        if (undo.Texture != m_PaintTexture || m_PaintImage.Empty())
        {
            Log::Warn("Undo: that texture paint step was on another texture");
            return;
        }
        for (PaintTile& t : undo.Tiles)
            for (int y = 0; y < t.H; ++y)
                for (int x = 0; x < t.W; ++x)
                    std::swap(m_PaintImage.At((uint32_t)(t.X + x), (uint32_t)(t.Y + y)), t.Pixels[(size_t)y * t.W + x]);
        AssetManager::SetTexturePixels(m_PaintTexture, m_PaintImage.Pixels.data(), m_PaintImage.Width, m_PaintImage.Height);
        m_PaintUnsaved = true;
    }

    // ---- Strokes

    bool MeshEditMode::PaintHit(const ImVec2& mouse, glm::vec3& point, uint32_t& face, glm::vec3& dir)
    {
        if (m_PaintRaysVersion != m_Mesh.Version())
        {
            m_PaintRays.Build(m_Mesh);
            m_PaintRaysVersion = m_Mesh.Version();
        }
        glm::vec3 from;
        SculptRay(mouse, from, dir);
        MeshRaycaster::RayHit hit = m_PaintRays.Raycast(from, dir);
        if (!hit.Hit) return false;
        point = hit.Point;
        face = hit.Face;
        return true;
    }

    pt::Brush MeshEditMode::PaintBrush(const glm::vec3& at, const glm::vec3& viewDir, bool blur) const
    {
        pt::Brush b;
        b.Color = m_PaintColor;
        b.Radius = m_PaintRadiusPx * ModelPerPixel(at);
        b.Strength = m_PaintStrength;
        b.Hardness = m_PaintHardness;
        b.Mode = blur ? pt::Blend::Blur : m_PaintBlend;
        b.FrontFacesOnly = m_PaintFrontFaces;
        b.ViewDir = viewDir;
        b.Symmetry = m_PaintSymmetry;
        return b;
    }

    void MeshEditMode::PaintDab(const glm::vec3& at, const glm::vec3& viewDir, bool blur)
    {
        pt::Brush b = PaintBrush(at, viewDir, blur);
        if (m_PaintMode == 1)
        {
            if (pt::PaintVertices(m_Mesh, at, b) > 0) Commit();
            return;
        }
        pt::Rect r = pt::PaintTexture(m_Mesh, m_PaintImage, at, b);
        if (r.Empty()) return;
        m_PaintStrokeRect.Add(r);
        m_PaintGpuDirty = m_PaintUnsaved = true;
    }

    void MeshEditMode::UpdatePaintStroke(bool hovered, bool blocked)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        bool overToolbar = mouse.x >= m_ToolbarMin.x && mouse.x <= m_ToolbarMax.x && mouse.y >= m_ToolbarMin.y && mouse.y <= m_ToolbarMax.y;
        glm::vec3 point, dir;
        uint32_t face;
        if (!m_Painting)
        {
            if (!(hovered && !blocked && !m_OverUI && !overToolbar && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return;
            if (m_PaintMode == 2 && m_PaintImage.Empty())
            {
                Log::Warn("Texture Paint: no texture to paint on (New Texture makes one)");
                return;
            }
            if (!PaintHit(mouse, point, face, dir)) return;
            Record(m_PaintMode == 1 ? "Vertex Paint" : "Texture Paint");
            m_Painting = true;
            m_PaintBlurStroke = io.KeyShift;
            m_PaintMouse = mouse;
            if (m_PaintMode == 2)
            {
                m_PaintBefore = m_PaintImage.Pixels;
                m_PaintStrokeRect = {};
            }
            PaintDab(point, dir, m_PaintBlurStroke);
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_Painting = false;
            if (m_PaintMode == 1)
            {
                Commit();
                return;
            }
            // The stroke's undo step: the tiles it changed, as they were.
            auto undo = std::make_shared<PaintUndo>();
            undo->Texture = m_PaintTexture;
            const pt::Rect& r = m_PaintStrokeRect;
            if (!r.Empty() && m_PaintBefore.size() == m_PaintImage.Pixels.size())
                for (int ty = r.Y0 / kTile; ty <= r.Y1 / kTile; ++ty)
                    for (int tx = r.X0 / kTile; tx <= r.X1 / kTile; ++tx)
                    {
                        PaintTile tile{ tx * kTile, ty * kTile, std::min(kTile, (int)m_PaintImage.Width - tx * kTile),
                                        std::min(kTile, (int)m_PaintImage.Height - ty * kTile), {} };
                        bool changed = false;
                        tile.Pixels.resize((size_t)tile.W * tile.H);
                        for (int y = 0; y < tile.H; ++y)
                            for (int x = 0; x < tile.W; ++x)
                            {
                                size_t i = (size_t)(tile.Y + y) * m_PaintImage.Width + tile.X + x;
                                tile.Pixels[(size_t)y * tile.W + x] = m_PaintBefore[i];
                                changed |= m_PaintBefore[i] != m_PaintImage.Pixels[i];
                            }
                        if (changed) undo->Tiles.push_back(std::move(tile));
                    }
            if (undo->Tiles.empty()) m_Undo.pop_back();
            else m_Undo.back().Paint = undo;
            m_PaintBefore.clear();
            m_PaintBefore.shrink_to_fit();
            return;
        }
        float spacing = std::max(2.0f, m_PaintRadiusPx * 0.2f);
        float length = Dist(mouse, m_PaintMouse);
        int steps = std::min((int)(length / spacing), 8);
        for (int i = 1; i <= steps; ++i)
        {
            float k = (float)i / (float)steps;
            ImVec2 at(m_PaintMouse.x + (mouse.x - m_PaintMouse.x) * k, m_PaintMouse.y + (mouse.y - m_PaintMouse.y) * k);
            if (PaintHit(at, point, face, dir)) PaintDab(point, dir, m_PaintBlurStroke);
        }
        if (steps > 0) m_PaintMouse = mouse;
    }

    // ---- Keys and view

    void MeshEditMode::HandlePaintKeys()
    {
        using Shortcuts::Pressed;
        if (Pressed("paint.radius")) m_PaintAdjust = 1;
        else if (Pressed("paint.strength")) m_PaintAdjust = 2;
        if (m_PaintAdjust)
        {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            m_PaintAdjustStart = m_PaintAdjust == 1 ? m_PaintRadiusPx : m_PaintStrength;
            float d = m_PaintAdjust == 1 ? m_PaintRadiusPx : m_PaintStrength * 200.0f;
            m_PaintAdjustCenter = ImVec2(mouse.x - d, mouse.y);
            m_PaintAdjustFrame = ImGui::GetFrameCount();
        }
        if (Pressed("paint.smaller")) m_PaintRadiusPx = std::max(2.0f, m_PaintRadiusPx / 1.15f);
        if (Pressed("paint.larger")) m_PaintRadiusPx = std::min(2000.0f, m_PaintRadiusPx * 1.15f);
        if (Pressed("paint.sample"))
        {
            // The color under the mouse.
            glm::vec3 point, dir;
            uint32_t face;
            glm::vec2 uv;
            glm::vec4 color;
            if (PaintHit(ImGui::GetIO().MousePos, point, face, dir) && pt::SurfaceAt(m_Mesh, face, point, uv, color))
                m_PaintColor = m_PaintMode == 2 && !m_PaintImage.Empty() ? m_PaintImage.Sample(uv) : color;
        }
    }

    bool MeshEditMode::UpdatePaintView(bool hovered, bool focused, bool blocked)
    {
        if (m_PaintMode == 2 && PaintTarget() != m_PaintTexture) LoadPaintTexture();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        DrawPaintToolbar(m_Origin);
        ImGui::SetCursorScreenPos(cursor);
        m_OverUI = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        if (m_PaintAdjust)
        {
            float d = Dist(mouse, m_PaintAdjustCenter);
            if (m_PaintAdjust == 1) m_PaintRadiusPx = std::clamp(d, 2.0f, 2000.0f);
            else m_PaintStrength = std::clamp(d / 200.0f, 0.0f, 1.0f);
            bool later = ImGui::GetFrameCount() > m_PaintAdjustFrame;
            if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
                m_PaintAdjust = 0;
            else if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
            {
                (m_PaintAdjust == 1 ? m_PaintRadiusPx : m_PaintStrength) = m_PaintAdjustStart;
                m_PaintAdjust = 0;
            }
        }
        else
        {
            if ((hovered || focused) && !m_Painting && !ImGui::GetIO().WantTextInput) HandlePaintKeys();
            if (m_PaintMode && !m_PaintAdjust) UpdatePaintStroke(hovered, blocked);
        }
        if (m_PaintGpuDirty && !m_PaintImage.Empty())
        {
            AssetManager::SetTexturePixels(m_PaintTexture, m_PaintImage.Pixels.data(), m_PaintImage.Width, m_PaintImage.Height);
            m_PaintGpuDirty = false;
        }
        // The brush: its circle in its color.
        glm::vec4 c = m_PaintColor;
        ImU32 col = IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 230);
        if (m_PaintAdjust)
        {
            dl->AddCircle(m_PaintAdjustCenter, m_PaintAdjust == 1 ? m_PaintRadiusPx : m_PaintStrength * 200.0f, col, 64, 2.0f);
            dl->AddCircle(m_PaintAdjustCenter, m_PaintRadiusPx, IM_COL32(255, 255, 255, 90), 64, 1.0f);
        }
        else if ((hovered || m_Painting) && !m_OverUI)
        {
            dl->AddCircle(mouse, m_PaintRadiusPx, IM_COL32(0, 0, 0, 160), 64, 3.0f);
            dl->AddCircle(mouse, m_PaintRadiusPx, col, 64, 1.5f);
        }
        char buf[300];
        std::snprintf(buf, sizeof(buf), "%s: %s   radius %.0f px (%s)   strength %.2f (%s)   %s: pick color   Shift: blur%s",
                      m_PaintMode == 1 ? "Vertex Paint" : "Texture Paint", pt::BlendName(m_PaintBlend), m_PaintRadiusPx, Shortcuts::Label("paint.radius").c_str(),
                      m_PaintStrength, Shortcuts::Label("paint.strength").c_str(), Shortcuts::Label("paint.sample").c_str(),
                      m_PaintMode == 2 && m_PaintUnsaved ? "   (texture unsaved: saved on Ctrl+S or leaving)" : "");
        dl->AddText(ImVec2(m_Origin.x + 8.0f, m_Origin.y + m_Size.y - 24.0f), IM_COL32(255, 255, 255, 230), buf);
        return m_Painting || m_PaintAdjust != 0;
    }

    void MeshEditMode::DrawPaintToolbar(const ImVec2& origin)
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
        if (toggle("Edit", false, "Back to Edit Mode")) SetPaintMode(0);
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Vertex Paint", m_PaintMode == 1, "Paint colors onto the model's corners (vertex colors)")) SetPaintMode(1);
        ImGui::SameLine(0.0f, 2.0f);
        if (toggle("Texture Paint", m_PaintMode == 2, "Paint onto the object's base color texture through its UVs")) SetPaintMode(2);
        ImGui::SameLine();
        ImGui::ColorEdit4("##paintcolor", &m_PaintColor.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        int blend = (int)m_PaintBlend;
        const char* blends[(int)pt::Blend::Count];
        for (int b = 0; b < (int)pt::Blend::Count; ++b) blends[b] = pt::BlendName((pt::Blend)b);
        if (ImGui::Combo("##blend", &blend, blends, m_PaintMode == 2 ? (int)pt::Blend::Count : (int)pt::Blend::Count)) m_PaintBlend = (pt::Blend)blend;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the color goes on (Shift while painting blurs)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat("##pradius", &m_PaintRadiusPx, 0.5f, 2.0f, 2000.0f, "%.0f px");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius on screen (%s; [ and ] too)", Shortcuts::Label("paint.radius").c_str());
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(70.0f);
        ImGui::SliderFloat("##pstrength", &m_PaintStrength, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strength (%s)", Shortcuts::Label("paint.strength").c_str());
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(70.0f);
        ImGui::SliderFloat("##phard", &m_PaintHardness, 0.0f, 1.0f, "hard %.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hardness: how far in from the rim the brush stays at full strength");
        ImGui::SameLine();
        if (toggle("Front Faces", m_PaintFrontFaces, "Only faces turned towards the camera")) m_PaintFrontFaces = !m_PaintFrontFaces;
        for (int a = 0; a < 3; ++a)
        {
            ImGui::SameLine(0.0f, 2.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%c##psym", "XYZ"[a]);
            if (toggle(label, (m_PaintSymmetry >> a) & 1, "Mirror strokes across the model's own plane")) m_PaintSymmetry ^= (uint8_t)(1 << a);
        }
        ImGui::SameLine();
        if (ImGui::Button("Done")) Exit();

        // Second row: what's painted on.
        if (m_PaintMode == 1)
        {
            if (ImGui::Button("Fill"))
            {
                std::vector<uint32_t> faces = UV::SelectedFaces(m_Mesh);
                if (faces.empty())
                    for (uint32_t f = 0; f < m_Mesh.FaceCount(); ++f) faces.push_back(f);
                Record("Fill Vertex Colors");
                pt::FillVertices(m_Mesh, faces, PackRGBA8(m_PaintColor));
                Commit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The selected faces (all when none is) in the color");
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                Record("Clear Vertex Colors");
                for (Face& f : m_Mesh.Faces) f.Colors.clear();
                Commit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes the vertex colors (all white)");
        }
        else
        {
            std::string name = m_PaintTexture.IsValid() ? AssetManager::GetDisplayName(m_PaintTexture) : std::string("(no texture)");
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s%s", name.c_str(), m_PaintImage.Empty() ? "" : (" " + std::to_string(m_PaintImage.Width) + "x" + std::to_string(m_PaintImage.Height)).c_str());
            ImGui::SameLine();
            if (ImGui::Button("New Texture...")) ImGui::OpenPopup("NewPaintTexture");
            if (ImGui::BeginPopup("NewPaintTexture"))
            {
                const char* sizes[] = { "256", "512", "1024", "2048", "4096" };
                int si = 0;
                while (si < 4 && std::atoi(sizes[si]) != m_NewTextureSize) ++si;
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::Combo("Size", &si, sizes, 5)) m_NewTextureSize = std::atoi(sizes[si]);
                ImGui::ColorEdit4("Fill", &m_NewTextureColor.x, ImGuiColorEditFlags_NoInputs);
                ImGui::TextDisabled("Saved in Assets/Textures and used as the object's base color texture");
                if (ImGui::Button("Create"))
                {
                    NewPaintTexture();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (!m_PaintImage.Empty())
            {
                ImGui::SameLine();
                if (ImGui::Button("Fill"))
                {
                    Record("Fill Texture");
                    m_PaintBefore = m_PaintImage.Pixels;
                    std::fill(m_PaintImage.Pixels.begin(), m_PaintImage.Pixels.end(), PackRGBA8(m_PaintColor));
                    auto undo = std::make_shared<PaintUndo>();
                    undo->Texture = m_PaintTexture;
                    undo->Tiles.push_back({ 0, 0, (int)m_PaintImage.Width, (int)m_PaintImage.Height, std::move(m_PaintBefore) });
                    m_Undo.back().Paint = undo;
                    m_PaintBefore = {};
                    m_PaintGpuDirty = m_PaintUnsaved = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The whole texture in the color");
                ImGui::SameLine();
                if (ImGui::Button("Save Texture")) SavePaintTexture();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Bake...")) ImGui::OpenPopup("BakeMaps");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bake normal, ambient occlusion and curvature maps for this model's UVs");
        DrawBakePopup();
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        m_ToolbarMin = ImGui::GetItemRectMin();
        m_ToolbarMax = ImGui::GetItemRectMax();
    }

    // ---- Baking

    void MeshEditMode::DrawBakePopup()
    {
        if (!ImGui::BeginPopup("BakeMaps")) return;
        ImGui::TextDisabled("Bake maps for %s's UVs", m_File.stem().string().c_str());
        // The detailed source: another object with a mesh, placed over this one in the scene.
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        std::string current = scene.IsValid(m_BakeSource) && reg.Has<TagComponent>(m_BakeSource) ? reg.Get<TagComponent>(m_BakeSource).Name : "None (this model itself)";
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("High Detail Source", current.c_str()))
        {
            if (ImGui::Selectable("None (this model itself)", !scene.IsValid(m_BakeSource))) m_BakeSource = Entity::Null();
            int row = 0;
            for (auto [e, mr, t] : reg.ViewOf<MeshRendererComponent, TransformComponent>())
            {
                if (e == m_Entity || !mr.Mesh.IsValid()) continue;
                std::string label = (reg.Has<TagComponent>(e) ? reg.Get<TagComponent>(e).Name : std::string("Object")) + "##" + std::to_string(row++);
                if (ImGui::Selectable(label.c_str(), e == m_BakeSource)) m_BakeSource = e;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A detailed version of this model (sculpted, subdivided...) placed over it: its surface is read along this one's normals");
        bool hasSource = scene.IsValid(m_BakeSource);
        ImGui::BeginDisabled(!hasSource);
        ImGui::Checkbox("Normal Map", &m_Bake.Normal);
        ImGui::EndDisabled();
        ImGui::Checkbox("Ambient Occlusion", &m_Bake.AmbientOcclusion);
        ImGui::Checkbox("Curvature", &m_Bake.Curvature);
        const char* sizes[] = { "256", "512", "1024", "2048", "4096" };
        int si = 0;
        while (si < 4 && (uint32_t)std::atoi(sizes[si]) != m_Bake.Width) ++si;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("Size", &si, sizes, 5)) m_Bake.Width = m_Bake.Height = (uint32_t)std::atoi(sizes[si]);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat("Cage Extrusion", &m_Bake.CageExtrusion, 0.001f, 0.0001f, 10.0f, "%.3f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far out (and in) from this surface the detailed one is looked for");
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderInt("AO Samples", &m_Bake.AOSamples, 4, 256);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat("AO Distance", &m_Bake.AODistance, 0.01f, 0.001f, 100.0f, "%.2f m");
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderInt("Margin", &m_Bake.Margin, 0, 64, "%d px");
        ImGui::Checkbox("Use the normal map on the object's material", &m_BakeAssign);
        ImGui::TextDisabled("Written to Assets/Textures/%s_Normal.png, _AO.png and _Curvature.png", m_File.stem().string().c_str());
        if (ImGui::Button("Bake"))
        {
            RunBake();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void MeshEditMode::RunBake()
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        MeshData low = m_Mesh.ToMeshData();
        std::shared_ptr<const MeshData> highData;
        glm::mat4 highToLow(1.0f);
        if (scene.IsValid(m_BakeSource) && reg.Has<MeshRendererComponent>(m_BakeSource))
        {
            AssetHandle mesh = reg.Get<MeshRendererComponent>(m_BakeSource).Mesh;
            highData = AssetManager::GetMeshData(mesh);
            if (!highData)
            {
                Log::Error("Bake: could not read the source object's mesh");
                return;
            }
            highToLow = glm::inverse(m_World) * reg.Get<TransformComponent>(m_BakeSource).World;
        }
        Bake::Settings settings = m_Bake;
        settings.Normal = settings.Normal && highData != nullptr;
        if (!settings.Normal && !settings.AmbientOcclusion && !settings.Curvature) return;
        Log::Info("Baking {}x{} maps...", settings.Width, settings.Height);
        Bake::Result r = Bake::BakeMaps(low, highData.get(), highToLow, settings);
        if (r.Texels == 0)
        {
            Log::Warn("Bake: the model's UVs cover no texels (unwrap it first)");
            return;
        }
        fs::path folder = AssetManager::GetAssetsDirectory() / "Textures";
        std::error_code ec;
        fs::create_directories(folder, ec);
        std::string stem = m_File.stem().string();
        AssetHandle normalMap = AssetHandle::None();
        auto write = [&](const pt::Image& image, const char* suffix) {
            if (image.Empty()) return AssetHandle::None();
            fs::path path = folder / (stem + suffix + ".png");
            if (!image.SavePng(path))
            {
                Log::Error("Could not write {}", path.string());
                return AssetHandle::None();
            }
            AssetHandle h = AssetManager::Import(path);
            if (h.IsValid())
            {
                // Data, not colors: read linearly.
                nlohmann::json settingsJson = AssetManager::GetMetadata(h) ? AssetManager::GetMetadata(h)->ImportSettings : nlohmann::json::object();
                settingsJson["srgb"] = false;
                AssetManager::SetImportSettings(h, settingsJson);
            }
            Log::Info("Baked {}", path.filename().string());
            return h;
        };
        normalMap = write(r.Normal, "_Normal");
        write(r.AmbientOcclusion, "_AO");
        write(r.Curvature, "_Curvature");
        if (highData && r.Missed > r.Texels / 20)
            Log::Warn("Bake: {} of {} texels found no detailed surface within the cage (try a larger Cage Extrusion)", r.Missed, r.Texels);
        if (m_BakeAssign && normalMap.IsValid())
        {
            auto& mr = reg.Get<MeshRendererComponent>(m_Entity);
            AssetHandle material = !mr.Materials.empty() && mr.Materials[0].IsValid() ? mr.Materials[0] : mr.Material;
            if (!material.IsValid())
            {
                // A material made from the renderer's own color and texture, with the normal map.
                fs::path matFolder = AssetManager::GetAssetsDirectory() / "Materials";
                fs::create_directories(matFolder, ec);
                fs::path path = matFolder / (stem + ".mat");
                for (int i = 1; fs::exists(path); ++i) path = matFolder / (stem + " " + std::to_string(i) + ".mat");
                material = AssetManager::CreateMaterial(path);
                if (Ref<Material> mat = AssetManager::GetMaterial(material))
                {
                    mat->BaseColor = mr.Color;
                    mat->BaseColorMap = mr.Texture;
                    mat->Save(path);
                }
                mr.Material = material;
                m_Ctx.MarkDirty();
            }
            if (Ref<Material> mat = AssetManager::GetMaterial(material); mat && !AssetManager::IsRuntimeMaterial(material))
            {
                mat->NormalMap = normalMap;
                mat->Save(AssetManager::GetAbsolutePath(material));
            }
        }
    }
}
