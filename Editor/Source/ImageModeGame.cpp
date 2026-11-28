// Image Mode's game art tools: the Swatches panel (palettes built in and the project's, in Assets/Palettes), the Sprite
// Sheet slicing written into the exported texture's settings, Export As (a format and a scale), and the tile and grid views.
#include "ImageMode.h"
#include "EditorUI.h"
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Core/Log.h>
#include <Dimension/Renderer/SpriteSlicing.h>
#include <Dimension/Renderer/Texture.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Imaging;

    namespace
    {
        constexpr int kScales[] = { 25, 50, 100, 200, 300, 400, 800 };
        constexpr const char* kScaleNames[] = { "25%", "50%", "100%", "200%", "300%", "400%", "800%" };
        constexpr const char* kFormats[] = { ".png", ".jpg", ".tga", ".bmp" };
    }

    // ---- Swatches

    void ImageMode::LoadPalettes()
    {
        m_Palettes = BuiltInPalettes();
        m_BuiltInPalettes = m_Palettes.size();
        fs::path dir = AssetManager::GetAssetsDirectory() / "Palettes";
        std::error_code ec;
        if (fs::is_directory(dir, ec))
            for (const auto& e : fs::directory_iterator(dir, ec))
            {
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                Palette p;
                if ((ext == ".gpl" || ext == ".hex" || ext == ".pal") && LoadPalette(e.path(), p)) m_Palettes.push_back(std::move(p));
            }
        m_PaletteIndex = std::clamp(m_PaletteIndex, 0, (int)m_Palettes.size() - 1);
        m_PalettesLoaded = true;
    }

    void ImageMode::DrawSwatches()
    {
        if (!m_PalettesLoaded) LoadPalettes();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##palette", m_Palettes[(size_t)m_PaletteIndex].Name.c_str()))
        {
            for (size_t i = 0; i < m_Palettes.size(); ++i)
            {
                if (i == m_BuiltInPalettes && i > 0) ImGui::Separator();
                std::string label = m_Palettes[i].Name + (i < m_BuiltInPalettes ? "" : "  (project)") + "##" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), (int)i == m_PaletteIndex)) m_PaletteIndex = (int)i;
            }
            ImGui::EndCombo();
        }
        Palette& pal = m_Palettes[(size_t)m_PaletteIndex];
        const bool own = (size_t)m_PaletteIndex >= m_BuiltInPalettes;
        // The colors: click picks the foreground, right-click (or Alt+click) the background; Ctrl+click removes one.
        const float cell = 22.0f;
        const int perRow = std::max(1, (int)((ImGui::GetContentRegionAvail().x + 2.0f) / (cell + 2.0f)));
        int remove = -1;
        for (size_t i = 0; i < pal.Colors.size(); ++i)
        {
            ImGui::PushID((int)i);
            glm::vec3 c = pal.Colors[i];
            if (ImGui::ColorButton("##swatch", ImVec4(c.r, c.g, c.b, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(cell, cell)))
            {
                if (ImGui::GetIO().KeyCtrl && own) remove = (int)i;
                else if (ImGui::GetIO().KeyAlt) m_Background = glm::vec4(c, 1.0f);
                else m_Foreground = glm::vec4(c, 1.0f);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_Background = glm::vec4(c, 1.0f);
            if (ImGui::IsItemHovered())
            {
                glm::ivec3 v = glm::ivec3(glm::round(c * 255.0f));
                ImGui::SetTooltip("#%02X%02X%02X  (click: foreground; right-click: background%s)", v.r, v.g, v.b, own ? "; Ctrl+click: remove" : "");
            }
            ImGui::PopID();
            if ((int)(i + 1) % perRow != 0) ImGui::SameLine(0.0f, 2.0f);
        }
        ImGui::NewLine();
        if (remove >= 0) pal.Colors.erase(pal.Colors.begin() + remove);
        // A palette of one's own: add the foreground color, name it, keep it in the project.
        auto makeOwn = [&](const std::string& name, std::vector<glm::vec3> colors) {
            Palette p;
            p.Name = name;
            p.Colors = std::move(colors);
            m_Palettes.push_back(std::move(p));
            m_PaletteIndex = (int)m_Palettes.size() - 1;
        };
        if (ImGui::Button("+ Color"))
        {
            if (own) pal.Colors.push_back(glm::vec3(m_Foreground));
            else
            {
                std::vector<glm::vec3> colors = pal.Colors;
                colors.push_back(glm::vec3(m_Foreground));
                makeOwn(pal.Name + " copy", std::move(colors));
            }
        }
        ImGui::SetItemTooltip("Add the foreground color (a built-in palette is copied first)");
        ImGui::SameLine();
        if (ImGui::Button("From Image")) makeOwn(GetName() + " colors", MedianCut(m_Composite, 16));
        ImGui::SetItemTooltip("A palette of the picture's 16 main colors");
        ImGui::SameLine();
        if (ImGui::Button("New")) makeOwn("New Palette", { glm::vec3(m_Foreground), glm::vec3(m_Background) });
        if (own)
        {
            Palette& p = m_Palettes[(size_t)m_PaletteIndex];
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60.0f);
            ImGui::InputText("##paletteName", &p.Name);
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(-FLT_MIN, 0)))
            {
                fs::path file = p.File.empty() ? AssetManager::GetAssetsDirectory() / "Palettes" / (p.Name + ".gpl") : fs::path(p.File);
                if (SavePalette(file, p))
                {
                    p.File = file.string();
                    Note("Saved the palette " + file.filename().string());
                }
                else
                    Note("Could not save the palette");
            }
            ImGui::SetItemTooltip("Into Assets/Palettes as a .gpl file (GIMP, Aseprite and others read it too)");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Image > Reduce Colors maps the picture to a palette.");
    }

    // ---- Sprite sheets and export

    void ImageMode::ComputeSpriteRects()
    {
        const int w = (int)m_Doc.Width, h = (int)m_Doc.Height;
        const std::vector<uint32_t>* pixels = m_SpriteSkipEmpty && m_Composite.Width == m_Doc.Width ? &m_Composite.Pixels : nullptr;
        switch (m_SpriteMode)
        {
        case 0: m_SpriteRects = SpriteSlicing::Grid(w, h, std::max(1, m_SpriteCount[0]), std::max(1, m_SpriteCount[1]), pixels); break;
        case 1:
            m_SpriteRects = SpriteSlicing::GridBySize(w, h, glm::ivec2(std::max(1, m_SpriteCell[0]), std::max(1, m_SpriteCell[1])),
                                                      glm::ivec2(m_SpriteOffset[0], m_SpriteOffset[1]), glm::ivec2(m_SpriteGap[0], m_SpriteGap[1]), pixels);
            break;
        default: m_SpriteRects = SpriteSlicing::Islands(m_Composite.Pixels, w, h, m_SpriteAlpha, m_SpriteMin); break;
        }
    }

    void ImageMode::ExportImage()
    {
        std::string name = m_ExportName.empty() ? GetName() : m_ExportName;
        fs::path file = m_File.parent_path() / name;
        if (!Bitmap::CanSave(file)) file.replace_extension(".png");
        Bitmap flat = m_Doc.Flatten();
        const int scale = kScales[std::clamp(m_ExportScale, 0, (int)std::size(kScales) - 1)];
        if (scale != 100)
            flat = ResampleBitmap(flat, std::max(1u, m_Doc.Width * (uint32_t)scale / 100u), std::max(1u, m_Doc.Height * (uint32_t)scale / 100u),
                                  m_ExportMethod == 0 ? Resample::Nearest : Resample::Bilinear);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (!flat.Save(file))
        {
            Note("Could not write " + file.filename().string());
            return;
        }
        AssetHandle texture = AssetManager::Import(file);
        if (texture.IsValid() && AssetManager::IsLoaded(texture)) AssetManager::SetTexturePixels(texture, flat.Pixels.data(), flat.Width, flat.Height);
        // From now on saving writes here (at 100%).
        if (m_ExportDefault && scale == 100)
        {
            m_Doc.Export = file.filename().string();
            m_Unsaved = true;
        }
        Note("Exported " + file.filename().string() + " (" + std::to_string(flat.Width) + " x " + std::to_string(flat.Height) + ")");
    }

    void ImageMode::DrawGameDialog()
    {
        const char* name = m_Dialog == 8 ? "Sprite Sheet" : "Export As";
        if (m_OpenDialog)
        {
            m_OpenDialog = false;
            if (m_Dialog == 8)
            {
                if (m_SpritePrefix.empty()) m_SpritePrefix = GetName();
                ComputeSpriteRects();
            }
            else
                m_ExportName = m_Doc.ExportPath(m_File).filename().string();
            ImGui::OpenPopup(name);
        }
        bool open = true;
        if (!ImGui::BeginPopupModal(name, &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (!ImGui::IsPopupOpen(name)) m_Dialog = 0, m_SpriteRects.clear();
            return;
        }
        bool close = !open;
        if (m_Dialog == 8)
        {
            bool changed = false;
            static const char* modes[] = { "Grid: rows and columns", "Grid: cell size", "Automatic (separate shapes)" };
            ImGui::SetNextItemWidth(240.0f);
            changed |= ImGui::Combo("Slice", &m_SpriteMode, modes, 3);
            ImGui::SetNextItemWidth(240.0f);
            if (m_SpriteMode == 0) changed |= ImGui::InputInt2("Columns, Rows", m_SpriteCount);
            else if (m_SpriteMode == 1)
            {
                ImGui::SetNextItemWidth(240.0f);
                changed |= ImGui::InputInt2("Cell Size", m_SpriteCell);
                ImGui::SetNextItemWidth(240.0f);
                changed |= ImGui::InputInt2("Offset", m_SpriteOffset);
                ImGui::SetNextItemWidth(240.0f);
                changed |= ImGui::InputInt2("Gap", m_SpriteGap);
            }
            else
            {
                changed |= ImGui::SliderInt("Alpha Threshold", &m_SpriteAlpha, 1, 254);
                ImGui::SetNextItemWidth(240.0f);
                changed |= ImGui::SliderInt("Smallest Size", &m_SpriteMin, 1, 64);
            }
            if (m_SpriteMode != 2) changed |= ImGui::Checkbox("Leave out empty cells", &m_SpriteSkipEmpty);
            ImGui::SetNextItemWidth(240.0f);
            ImGui::InputText("Names", &m_SpritePrefix);
            if (changed) ComputeSpriteRects();
            ImGui::Text("%d sprites (outlined on the image)", (int)m_SpriteRects.size());
            const fs::path out = m_Doc.ExportPath(m_File);
            ImGui::TextDisabled("They go into %s's settings: Sprite Renderers pick them by name.", out.filename().string().c_str());
            ImGui::BeginDisabled(m_SpriteRects.empty());
            if (ImGui::Button("Apply to the Texture"))
            {
                Save(); // (the picture is written first, so the texture is there)
                AssetHandle texture = AssetManager::Import(out);
                if (const AssetMetadata* meta = texture.IsValid() ? AssetManager::GetMetadata(texture) : nullptr)
                {
                    std::vector<SpriteRect> sprites;
                    for (size_t i = 0; i < m_SpriteRects.size(); ++i) sprites.push_back({ m_SpritePrefix + "_" + std::to_string(i), m_SpriteRects[i] });
                    nlohmann::json settings = meta->ImportSettings;
                    settings["sprites"] = SpriteSlicing::ToJson(sprites);
                    AssetManager::SetImportSettings(texture, settings);
                    Note(std::to_string(sprites.size()) + " sprites on " + out.filename().string() + " (Window > Sprite Editor fine-tunes them)");
                    close = true;
                }
                else
                    Note("Could not find the texture " + out.filename().string());
            }
            ImGui::SameLine();
            if (ImGui::Button("Export Each as PNG"))
            {
                fs::path dir = m_File.parent_path() / (m_SpritePrefix + "_Sprites");
                std::error_code ec;
                fs::create_directories(dir, ec);
                Bitmap flat = m_Doc.Flatten();
                int written = 0;
                for (size_t i = 0; i < m_SpriteRects.size(); ++i)
                {
                    const glm::ivec4 r = m_SpriteRects[i];
                    Bitmap piece = Bitmap::Solid((uint32_t)std::max(1, r.z), (uint32_t)std::max(1, r.w), 0u);
                    for (int y = 0; y < r.w; ++y)
                        for (int x = 0; x < r.z; ++x)
                            if (r.x + x < (int)flat.Width && r.y + y < (int)flat.Height) piece.At((uint32_t)x, (uint32_t)y) = flat.At((uint32_t)(r.x + x), (uint32_t)(r.y + y));
                    fs::path file = dir / (m_SpritePrefix + "_" + std::to_string(i) + ".png");
                    if (piece.Save(file)) ++written, AssetManager::Import(file);
                }
                Note("Wrote " + std::to_string(written) + " sprites into " + dir.filename().string());
                close = true;
            }
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::SetNextItemWidth(260.0f);
            ImGui::InputText("File Name", &m_ExportName);
            ImGui::SameLine();
            ImGui::TextDisabled("(.png .jpg .tga .bmp)");
            for (const char* ext : kFormats)
            {
                if (ImGui::SmallButton(ext)) m_ExportName = fs::path(m_ExportName.empty() ? GetName() : m_ExportName).replace_extension(ext).string();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Scale", &m_ExportScale, kScaleNames, (int)std::size(kScaleNames));
            ImGui::SameLine();
            static const char* methods[] = { "Nearest (pixel art)", "Bilinear (smooth)" };
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo("##exportMethod", &m_ExportMethod, methods, 2);
            const int scale = kScales[std::clamp(m_ExportScale, 0, (int)std::size(kScales) - 1)];
            ImGui::TextDisabled("%u x %u px, next to the image", m_Doc.Width * (uint32_t)scale / 100u, m_Doc.Height * (uint32_t)scale / 100u);
            ImGui::BeginDisabled(scale != 100);
            ImGui::Checkbox("Saving writes this file from now on", &m_ExportDefault);
            ImGui::EndDisabled();
            if (ImGui::Button("Export", ImVec2(120, 0)))
            {
                ExportImage();
                close = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100, 0))) close = true;
        if (close)
        {
            ImGui::CloseCurrentPopup();
            m_Dialog = 0;
            m_SpriteRects.clear();
        }
        ImGui::EndPopup();
    }

    // ---- Views

    void ImageMode::DrawGameOverlay(ImDrawList* dl)
    {
        const ImVec2 c0 = m_CanvasMin;
        const ImVec2 size(m_Doc.Width * m_Zoom, m_Doc.Height * m_Zoom);
        auto screen = [&](float x, float y) { return ImVec2(c0.x + x * m_Zoom, c0.y + y * m_Zoom); };
        // The picture repeated around itself, to see how it tiles.
        if (m_TilePreview && m_Texture.IsValid())
            if (Ref<Texture2D> tex = AssetManager::GetTexture(m_Texture))
                for (int ty = -1; ty <= 1; ++ty)
                    for (int tx = -1; tx <= 1; ++tx)
                    {
                        if (!tx && !ty) continue;
                        ImVec2 a(c0.x + tx * size.x, c0.y + ty * size.y);
                        dl->AddImage(ImTextureRef((ImTextureID)tex->GetImGuiId()), a, ImVec2(a.x + size.x, a.y + size.y), ImVec2(0, 0), ImVec2(1, 1),
                                     IM_COL32(235, 235, 235, 255));
                    }
        // A grid of cells (tiles, sprite frames).
        if (m_ShowGrid && m_GridSize[0] * m_Zoom >= 4.0f && m_GridSize[1] * m_Zoom >= 4.0f)
        {
            const ImU32 col = IM_COL32(0, 200, 255, 110);
            for (int x = m_GridSize[0]; x < (int)m_Doc.Width; x += m_GridSize[0]) dl->AddLine(screen((float)x, 0), screen((float)x, (float)m_Doc.Height), col);
            for (int y = m_GridSize[1]; y < (int)m_Doc.Height; y += m_GridSize[1]) dl->AddLine(screen(0, (float)y), screen((float)m_Doc.Width, (float)y), col);
        }
        // The brushes' mirror lines.
        if (m_Symmetry && ToolBrush(m_Tool))
        {
            const ImU32 col = IM_COL32(255, 80, 200, 200);
            if (m_Symmetry & 1) dl->AddLine(screen(m_Doc.Width * 0.5f, 0), screen(m_Doc.Width * 0.5f, (float)m_Doc.Height), col, 1.5f);
            if (m_Symmetry & 2) dl->AddLine(screen(0, m_Doc.Height * 0.5f), screen((float)m_Doc.Width, m_Doc.Height * 0.5f), col, 1.5f);
        }
        // The sprites being sliced.
        for (size_t i = 0; i < m_SpriteRects.size(); ++i)
        {
            const glm::ivec4 r = m_SpriteRects[i];
            ImVec2 a = screen((float)r.x, (float)r.y), b = screen((float)(r.x + r.z), (float)(r.y + r.w));
            dl->AddRect(a, b, IM_COL32(0, 0, 0, 200), 0.0f, 0, 3.0f);
            dl->AddRect(a, b, IM_COL32(255, 220, 0, 255), 0.0f, 0, 1.0f);
            if (b.x - a.x > 24.0f) dl->AddText(ImVec2(a.x + 3, a.y + 2), IM_COL32(255, 220, 0, 255), std::to_string(i).c_str());
        }
    }
}
