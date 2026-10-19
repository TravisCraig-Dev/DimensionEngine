#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Platform/FileDialogs.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace Editor
{
    namespace
    {
        std::string Lower(std::string s)
        {
            for (auto& c : s) c = (char)std::tolower(c);
            return s;
        }

        fs::path UniquePath(const fs::path& desired)
        {
            fs::path p = desired;
            for (int i = 1; fs::exists(p); ++i)
                p = desired.parent_path() / (desired.stem().string() + " " + std::to_string(i) + desired.extension().string());
            return p;
        }
    }

    void ContentBrowserPanel::ImportFiles(const std::vector<fs::path>& files)
    {
        fs::path target = m_Current.empty() ? AssetManager::GetAssetsDirectory() : m_Current;
        for (auto& f : files)
        {
            std::error_code ec;
            fs::path dest = UniquePath(target / f.filename());
            if (fs::is_directory(f)) fs::copy(f, dest, fs::copy_options::recursive, ec);
            else fs::copy_file(f, dest, ec);
            if (ec) Log::Error("Import of '{}' failed: {}", f.string(), ec.message());
            else Log::Info("Imported '{}'", dest.filename().string());
        }
        AssetManager::Refresh();
    }

    void ContentBrowserPanel::DrawFolderTree(const fs::path& dir)
    {
        std::vector<fs::path> subdirs;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec))
            if (entry.is_directory()) subdirs.push_back(entry.path());
        std::sort(subdirs.begin(), subdirs.end());

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (subdirs.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (dir == m_Current) flags |= ImGuiTreeNodeFlags_Selected;
        if (dir == AssetManager::GetAssetsDirectory()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        bool open = ImGui::TreeNodeEx(dir.string().c_str(), flags, "%s", dir.filename().string().c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_Current = dir;
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kPathPayload))
            {
                fs::path from = fs::path(std::u8string((const char8_t*)p->Data, (const char8_t*)p->Data + p->DataSize));
                if (from.parent_path() != dir) AssetManager::MoveAsset(from, dir / from.filename());
            }
            ImGui::EndDragDropTarget();
        }
        if (open)
        {
            for (auto& d : subdirs) DrawFolderTree(d);
            ImGui::TreePop();
        }
    }

    void ContentBrowserPanel::OnImGui(bool* open)
    {
        if (!ImGui::Begin("Project", open))
        {
            ImGui::End();
            return;
        }
        const fs::path& root = AssetManager::GetAssetsDirectory();
        if (m_Current.empty() || !fs::exists(m_Current)) m_Current = root;

        if (ImGui::BeginTable("ContentLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("FolderTree");
            DrawFolderTree(root);
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            // Toolbar: back, breadcrumb, search, size
            ImGui::BeginDisabled(m_Current == root);
            if (ImGui::Button("<")) m_Current = m_Current.parent_path();
            ImGui::EndDisabled();
            ImGui::SameLine();
            fs::path rel = m_Current.lexically_relative(root.parent_path());
            fs::path accumulated = root.parent_path();
            bool first = true;
            for (auto& part : rel)
            {
                accumulated /= part;
                if (!first) { ImGui::SameLine(0, 2); ImGui::TextDisabled(">"); ImGui::SameLine(0, 2); }
                first = false;
                if (ImGui::SmallButton(part.string().c_str())) m_Current = accumulated;
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 330);
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##search", "Search", &m_Search);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("##size", &m_ThumbnailSize, 48.0f, 160.0f, "%.0f px");
            ImGui::Separator();

            ImGui::BeginChild("Files");
            std::vector<fs::directory_entry> entries;
            std::error_code ec;
            std::string search = Lower(m_Search);
            if (!search.empty())
            {
                for (auto& e : fs::recursive_directory_iterator(root, ec))
                    if (e.is_regular_file() && e.path().extension() != ".meta" && Lower(e.path().filename().string()).find(search) != std::string::npos)
                        entries.push_back(e);
            }
            else
                for (auto& e : fs::directory_iterator(m_Current, ec))
                    if (e.path().extension() != ".meta") entries.push_back(e);
            std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
                if (a.is_directory() != b.is_directory()) return a.is_directory();
                return Lower(a.path().filename().string()) < Lower(b.path().filename().string());
            });

            float cell = m_ThumbnailSize + 16.0f;
            int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
            fs::path toDelete, enterDir;
            if (ImGui::BeginTable("Grid", columns))
            {
                for (auto& entry : entries)
                {
                    const fs::path& path = entry.path();
                    bool isDir = entry.is_directory();
                    AssetHandle handle = isDir ? AssetHandle::None() : AssetManager::GetHandle(path);
                    const AssetMetadata* meta = AssetManager::GetMetadata(handle);
                    AssetType type = meta ? meta->Type : AssetType::None;
                    std::string name = isDir ? path.filename().string() : path.stem().string();

                    ImGui::TableNextColumn();
                    ImGui::PushID(path.string().c_str());
                    ImVec2 size(m_ThumbnailSize, m_ThumbnailSize);
                    bool selected = handle.IsValid() && m_Ctx.SelectedAsset == handle;

                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("thumb", size);
                    bool hovered = ImGui::IsItemHovered();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 max(cursor.x + size.x, cursor.y + size.y);
                    if (selected || hovered)
                        dl->AddRectFilled(ImVec2(cursor.x - 3, cursor.y - 3), ImVec2(max.x + 3, max.y + 3),
                                          ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), 4.0f);

                    Ref<Texture2D> tex = type == AssetType::Texture ? AssetManager::GetTexture(handle) : nullptr;
                    Ref<Material> mat = type == AssetType::Material ? AssetManager::GetMaterial(handle) : nullptr;
                    if (mat)
                    {
                        // Swatch: base map (if any) tinted by the base color, drawn as a circle.
                        ImVec2 c(cursor.x + size.x * 0.5f, cursor.y + size.y * 0.5f);
                        float r = size.x * 0.42f;
                        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(mat->BaseColor.r, mat->BaseColor.g, mat->BaseColor.b, 1.0f));
                        if (auto base = AssetManager::GetTexture(mat->BaseColorMap))
                            dl->AddImageRounded(ImTextureRef((ImTextureID)base->GetImGuiId()), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                                                ImVec2(0, 0), ImVec2(1, 1), col, r);
                        else
                            dl->AddCircleFilled(c, r, col, 48);
                        dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), r * 0.18f,
                                            IM_COL32(255, 255, 255, (int)(160 * (1.0f - mat->Roughness))), 24);
                        dl->AddCircle(c, r, IM_COL32(0, 0, 0, 120), 48, 1.5f);
                    }
                    else if (tex)
                    {
                        float aspect = (float)tex->GetWidth() / std::max(1u, tex->GetHeight());
                        ImVec2 imgSize = aspect >= 1.0f ? ImVec2(size.x, size.y / aspect) : ImVec2(size.x * aspect, size.y);
                        ImVec2 off((size.x - imgSize.x) * 0.5f, (size.y - imgSize.y) * 0.5f);
                        dl->AddImage(ImTextureRef((ImTextureID)tex->GetImGuiId()), ImVec2(cursor.x + off.x, cursor.y + off.y),
                                     ImVec2(cursor.x + off.x + imgSize.x, cursor.y + off.y + imgSize.y));
                    }
                    else
                    {
                        ImVec4 c = isDir ? ImVec4(0.85f, 0.72f, 0.35f, 1) : UI::AssetColor(type);
                        if (isDir)
                        {
                            // Folder glyph
                            float t = size.y * 0.18f;
                            dl->AddRectFilled(ImVec2(cursor.x + 6, cursor.y + t), ImVec2(cursor.x + size.x * 0.45f, cursor.y + t * 2), ImGui::GetColorU32(c), 3.0f);
                            dl->AddRectFilled(ImVec2(cursor.x + 6, cursor.y + t * 1.6f), ImVec2(max.x - 6, max.y - t * 0.6f), ImGui::GetColorU32(c), 4.0f);
                        }
                        else
                        {
                            dl->AddRectFilled(ImVec2(cursor.x + 8, cursor.y + 4), ImVec2(max.x - 8, max.y - 4), ImGui::GetColorU32(ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1)), 4.0f);
                            dl->AddRect(ImVec2(cursor.x + 8, cursor.y + 4), ImVec2(max.x - 8, max.y - 4), ImGui::GetColorU32(c), 4.0f, 0, 2.0f);
                            const char* letter = UI::AssetLetter(type);
                            ImVec2 ts = ImGui::CalcTextSize(letter);
                            dl->AddText(ImVec2(cursor.x + (size.x - ts.x) * 0.5f, cursor.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(c), letter);
                        }
                    }

                    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && handle.IsValid()) m_Ctx.SelectAsset(handle);
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (isDir) enterDir = path;
                        else if (type == AssetType::Scene) m_Ctx.RequestOpenScene = path;
                        else if (type == AssetType::Prefab && handle.IsValid()) m_Ctx.RequestOpenPrefab = handle;
                        else FileDialogs::OpenWithDefaultApp(path);
                    }

                    if (ImGui::BeginDragDropSource())
                    {
                        if (handle.IsValid())
                        {
                            uint64_t h = handle;
                            ImGui::SetDragDropPayload(UI::kAssetPayload, &h, sizeof(h));
                        }
                        else
                        {
                            std::u8string p8 = path.u8string();
                            ImGui::SetDragDropPayload(UI::kPathPayload, p8.data(), p8.size());
                        }
                        ImGui::Text("%s", name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (isDir && ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kAssetPayload))
                        {
                            fs::path from = AssetManager::GetAbsolutePath(AssetHandle(*(const uint64_t*)p->Data));
                            if (!from.empty()) AssetManager::MoveAsset(from, path / from.filename());
                        }
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kPathPayload))
                        {
                            fs::path from = fs::path(std::u8string((const char8_t*)p->Data, (const char8_t*)p->Data + p->DataSize));
                            if (from != path) AssetManager::MoveAsset(from, path / from.filename());
                        }
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kEntityPayload))
                        {
                            Entity e = m_Ctx.ActiveScene().FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                            if (!e.IsNull())
                            {
                                m_Ctx.RecordUndo();
                                Prefabs::CreateFromEntity(m_Ctx.ActiveScene(), e, UniquePath(path / (m_Ctx.ActiveScene().GetRegistry().Get<TagComponent>(e).Name + ".prefab")));
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (ImGui::BeginPopupContextItem("ItemContext"))
                    {
                        if (ImGui::MenuItem("Rename")) { m_Renaming = path; m_RenameBuffer = path.filename().string(); m_RenameFocus = true; }
                        if (ImGui::MenuItem("Show in Explorer")) FileDialogs::RevealInExplorer(path);
                        if (ImGui::MenuItem("Delete")) toDelete = path;
                        ImGui::EndPopup();
                    }

                    // Label / rename field
                    if (m_Renaming == path)
                    {
                        ImGui::SetNextItemWidth(m_ThumbnailSize);
                        if (m_RenameFocus)
                        {
                            ImGui::SetKeyboardFocusHere();
                            m_RenameFocus = false;
                        }
                        if (ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                        {
                            fs::path dest = path.parent_path() / std::u8string_view((const char8_t*)m_RenameBuffer.c_str());
                            if (dest != path) AssetManager::MoveAsset(path, dest);
                            m_Renaming.clear();
                        }
                        else if (ImGui::IsItemDeactivated()) m_Renaming.clear(); // clicked away or Escape
                    }
                    else
                    {
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_ThumbnailSize);
                        ImGui::TextWrapped("%s", name.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // Drop entities onto the empty area to create prefabs in the current folder.
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 40.0f)));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kEntityPayload))
                {
                    Entity e = m_Ctx.ActiveScene().FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                    if (!e.IsNull())
                    {
                        std::string name = m_Ctx.ActiveScene().GetRegistry().Get<TagComponent>(e).Name;
                        m_Ctx.RecordUndo();
                        Prefabs::CreateFromEntity(m_Ctx.ActiveScene(), e, UniquePath(m_Current / (name + ".prefab")));
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextWindow("FilesContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
            {
                if (ImGui::MenuItem("New Folder"))
                {
                    fs::path p = UniquePath(m_Current / "New Folder");
                    fs::create_directory(p, ec);
                    m_Renaming = p;
                    m_RenameBuffer = p.filename().string();
                    m_RenameFocus = true;
                }
                if (ImGui::MenuItem("New Material"))
                {
                    fs::path p = UniquePath(m_Current / "New Material.mat");
                    AssetHandle h = AssetManager::CreateMaterial(p);
                    if (h.IsValid()) m_Ctx.SelectAsset(h);
                }
                if (ImGui::MenuItem("New Animator Controller"))
                {
                    AssetHandle h = AssetManager::CreateAnimatorController(UniquePath(m_Current / "New Animator.controller"));
                    if (h.IsValid()) m_Ctx.SelectAsset(h);
                }
                if (ImGui::MenuItem("New Scene"))
                {
                    fs::path p = UniquePath(m_Current / "New Scene.scene");
                    Scene empty("New Scene");
                    SceneSerializer::SaveToFile(empty, p);
                    AssetManager::Refresh();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Show in Explorer")) FileDialogs::OpenWithDefaultApp(m_Current);
                if (ImGui::MenuItem("Refresh")) AssetManager::Refresh();
                ImGui::EndPopup();
            }
            ImGui::EndChild();
            ImGui::EndTable();

            if (!enterDir.empty()) m_Current = enterDir, m_Search.clear();
            if (!toDelete.empty() && FileDialogs::Confirm("Delete Asset", "Delete '" + toDelete.filename().string() + "'? This cannot be undone."))
            {
                if (m_Ctx.SelectedAsset == AssetManager::GetHandle(toDelete)) m_Ctx.SelectAsset(AssetHandle::None());
                AssetManager::DeleteAsset(toDelete);
            }
        }
        ImGui::End();
    }
}
