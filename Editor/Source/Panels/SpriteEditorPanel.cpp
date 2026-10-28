#include "Panels.h"
#include <Dimension/Renderer/SpriteSlicing.h>
#include <Dimension/Renderer/Texture.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace Editor
{
    void SpriteEditorPanel::Open(AssetHandle texture)
    {
        m_Texture = texture;
        m_Sprites = AssetManager::GetSprites(texture);
        m_Selected = m_Sprites.empty() ? -1 : 0;
        m_Dirty = false;
        m_Pixels.clear();
        m_Width = m_Height = 0;
        SpriteSlicing::LoadPixels(AssetManager::GetAbsolutePath(texture), m_Pixels, m_Width, m_Height);
    }

    // Window > Sprite Editor (like Unity's): the texture selected in the Project window, sliced into named sprites by
    // a grid (a number of cells, or cells of a size) or automatically (each group of touching opaque pixels); each
    // sprite's name and rectangle can be edited, sprites added and removed, and Apply saves them with the texture.
    // Sprite Renderers then pick one by name.
    void SpriteEditorPanel::OnImGui(bool* open)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(820.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Sprite Editor", open))
        {
            ImGui::End();
            return;
        }
        // Follows the texture selected in the Project window (unless there are changes not applied).
        const AssetMetadata* selected = AssetManager::GetMetadata(m_Ctx.SelectedAsset);
        if (selected && selected->Type == AssetType::Texture && m_Ctx.SelectedAsset != m_Texture && !m_Dirty) Open(m_Ctx.SelectedAsset);
        const AssetMetadata* meta = AssetManager::GetMetadata(m_Texture);
        Ref<Texture2D> tex = AssetManager::GetTexture(m_Texture);
        if (!meta || !tex || m_Width <= 0)
        {
            ImGui::TextDisabled("Select a texture in the Project window to slice it into sprites.");
            ImGui::End();
            return;
        }
        std::string stem = meta->Path.stem().string();
        ImGui::Text("%s", meta->Path.filename().string().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d, %zu sprite(s)%s", m_Width, m_Height, m_Sprites.size(), m_Dirty ? "  (not applied)" : "");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f + ImGui::GetCursorPosX());
        ImGui::BeginDisabled(!m_Dirty);
        if (ImGui::Button("Revert")) Open(m_Texture);
        ImGui::SameLine();
        if (ImGui::Button("Apply"))
        {
            nlohmann::json settings = meta->ImportSettings;
            settings["sprites"] = SpriteSlicing::ToJson(m_Sprites);
            AssetManager::SetImportSettings(m_Texture, settings);
            m_Dirty = false;
        }
        ImGui::EndDisabled();

        // Left: the texture with the sprites outlined (click one to pick it). Right: slicing and the picked sprite.
        float side = 290.0f;
        ImVec2 area(std::max(ImGui::GetContentRegionAvail().x - side - 8.0f, 64.0f), std::max(ImGui::GetContentRegionAvail().y, 64.0f));
        ImGui::BeginChild("##image", area, ImGuiChildFlags_Borders);
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float scale = std::min(avail.x / m_Width, avail.y / m_Height);
            ImVec2 size(m_Width * scale, m_Height * scale);
            ImVec2 at = ImGui::GetCursorScreenPos();
            at.x += (avail.x - size.x) * 0.5f;
            at.y += (avail.y - size.y) * 0.5f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int y = 0; y < (int)(size.y / 12.0f) + 1; ++y) // a checkerboard behind, so transparency shows
                for (int x = 0; x < (int)(size.x / 12.0f) + 1; ++x)
                    dl->AddRectFilled(ImVec2(at.x + x * 12.0f, at.y + y * 12.0f), ImVec2(std::min(at.x + x * 12.0f + 12.0f, at.x + size.x), std::min(at.y + y * 12.0f + 12.0f, at.y + size.y)),
                                      (x + y) % 2 ? IM_COL32(70, 70, 70, 255) : IM_COL32(95, 95, 95, 255));
            dl->AddImage(ImTextureRef((ImTextureID)tex->GetImGuiId()), at, ImVec2(at.x + size.x, at.y + size.y));
            for (int i = 0; i < (int)m_Sprites.size(); ++i)
            {
                const glm::ivec4& r = m_Sprites[i].Rect;
                ImVec2 a(at.x + r.x * scale, at.y + r.y * scale), b(at.x + (r.x + r.z) * scale, at.y + (r.y + r.w) * scale);
                bool picked = i == m_Selected;
                dl->AddRect(a, b, picked ? IM_COL32(255, 210, 60, 255) : IM_COL32(90, 200, 255, 200), 0.0f, 0, picked ? 2.0f : 1.0f);
            }
            ImGui::SetCursorScreenPos(at);
            ImGui::InvisibleButton("##pick", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
            if (ImGui::IsItemClicked())
            {
                ImVec2 m = ImGui::GetMousePos();
                glm::vec2 p((m.x - at.x) / scale, (m.y - at.y) / scale);
                m_Selected = -1;
                for (int i = (int)m_Sprites.size() - 1; i >= 0; --i)
                {
                    const glm::ivec4& r = m_Sprites[i].Rect;
                    if (p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w)
                    {
                        m_Selected = i;
                        break;
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##side", ImVec2(side, area.y));
        {
            ImGui::SeparatorText("Slice");
            const char* modes[] = { "Grid by Cell Count", "Grid by Cell Size", "Automatic" };
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##mode", &m_Mode, modes, IM_ARRAYSIZE(modes));
            if (m_Mode == 0)
            {
                ImGui::SetNextItemWidth(120.0f);
                ImGui::DragInt2("Columns, Rows", &m_Cells.x, 0.1f, 1, 256);
            }
            else if (m_Mode == 1)
            {
                ImGui::SetNextItemWidth(120.0f);
                ImGui::DragInt2("Cell Size", &m_CellSize.x, 0.2f, 1, 8192);
                ImGui::SetNextItemWidth(120.0f);
                ImGui::DragInt2("Offset", &m_Offset.x, 0.2f, 0, 8192);
                ImGui::SetNextItemWidth(120.0f);
                ImGui::DragInt2("Gap", &m_Gap.x, 0.2f, 0, 8192);
            }
            else
            {
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderInt("Alpha Above", &m_AlphaThreshold, 0, 254);
                ImGui::SetNextItemWidth(120.0f);
                ImGui::DragInt("Smallest", &m_MinSize, 0.1f, 1, 1024, "%d px");
            }
            if (m_Mode != 2) ImGui::Checkbox("Leave out empty cells", &m_SkipEmpty);
            if (ImGui::Button("Slice", ImVec2(-1.0f, 0.0f)))
            {
                const std::vector<uint32_t>* pixels = m_SkipEmpty ? &m_Pixels : nullptr;
                std::vector<glm::ivec4> rects = m_Mode == 0 ? SpriteSlicing::Grid(m_Width, m_Height, m_Cells.x, m_Cells.y, pixels)
                                              : m_Mode == 1 ? SpriteSlicing::GridBySize(m_Width, m_Height, m_CellSize, m_Offset, m_Gap, pixels)
                                                            : SpriteSlicing::Islands(m_Pixels, m_Width, m_Height, m_AlphaThreshold, m_MinSize);
                m_Sprites.clear();
                for (size_t i = 0; i < rects.size(); ++i) m_Sprites.push_back({ stem + "_" + std::to_string(i), rects[i] });
                m_Selected = m_Sprites.empty() ? -1 : 0;
                m_Dirty = true;
            }
            ImGui::SeparatorText("Sprites");
            if (ImGui::Button("+ Add"))
            {
                m_Sprites.push_back({ stem + "_" + std::to_string(m_Sprites.size()), glm::ivec4(0, 0, m_Width, m_Height) });
                m_Selected = (int)m_Sprites.size() - 1;
                m_Dirty = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(m_Sprites.empty());
            if (ImGui::Button("Clear"))
            {
                m_Sprites.clear();
                m_Selected = -1;
                m_Dirty = true;
            }
            ImGui::EndDisabled();
            if (m_Selected >= 0 && m_Selected < (int)m_Sprites.size())
            {
                SpriteRect& s = m_Sprites[m_Selected];
                ImGui::SetNextItemWidth(-60.0f);
                m_Dirty |= ImGui::InputText("Name", &s.Name);
                ImGui::SetNextItemWidth(-60.0f);
                if (ImGui::DragInt4("Rect", &s.Rect.x, 0.2f, 0, 8192))
                {
                    s.Rect.x = std::clamp(s.Rect.x, 0, m_Width - 1);
                    s.Rect.y = std::clamp(s.Rect.y, 0, m_Height - 1);
                    s.Rect.z = std::clamp(s.Rect.z, 1, m_Width - s.Rect.x);
                    s.Rect.w = std::clamp(s.Rect.w, 1, m_Height - s.Rect.y);
                    m_Dirty = true;
                }
                ImGui::TextDisabled("x, y from the top-left; width, height (pixels)");
                if (ImGui::Button("Remove"))
                {
                    m_Sprites.erase(m_Sprites.begin() + m_Selected);
                    m_Selected = std::min(m_Selected, (int)m_Sprites.size() - 1);
                    m_Dirty = true;
                }
            }
            ImGui::BeginChild("##list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            for (int i = 0; i < (int)m_Sprites.size(); ++i)
            {
                ImGui::PushID(i);
                const glm::ivec4& r = m_Sprites[i].Rect;
                if (ImGui::Selectable(m_Sprites[i].Name.c_str(), i == m_Selected)) m_Selected = i;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f + ImGui::GetCursorPosX());
                ImGui::TextDisabled("%dx%d", r.z, r.w);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::End();
    }
}
