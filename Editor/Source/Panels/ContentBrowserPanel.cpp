#include "../AssetPreviews.h"
#include <fstream>
#include <Dimension/Asset/AssetPack.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Renderer/PostProcess.h>
#include <Dimension/Renderer/SpriteAtlas.h>
#include <Dimension/Asset/DataAsset.h>
#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scene/SceneSearch.h>
#include <Dimension/Asset/AssetDependencies.h>
#include <Dimension/Asset/AssetListing.h>
#include <Dimension/Asset/ProjectFavorites.h>
#include "../EditorPreferences.h"
#include <optional>
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

        // A button drawn pressed while `on`.
        bool ToggleButton(const char* label, bool on)
        {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool clicked = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            return clicked;
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
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_Current = dir, m_Search.clear();
        if (ImGui::BeginPopupContextItem("FolderContext"))
        {
            if (ImGui::MenuItem("Add to Favorites")) ProjectFavorites::AddFolder(dir), m_FavoritesFrame = -1000;
            if (ImGui::MenuItem("Show in Explorer")) FileDialogs::OpenWithDefaultApp(dir);
            ImGui::EndPopup();
        }
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

    void ContentBrowserPanel::ItemBehavior(const fs::path& path, const std::string& name, bool isDir, AssetHandle handle, AssetType type, bool hovered)
    {
        // Ctrl click adds or removes an asset; Shift click selects the ones from the last clicked to it.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && handle.IsValid())
        {
            if (ImGui::GetIO().KeyCtrl) m_Ctx.ToggleSelectAsset(handle), m_AssetAnchor = handle;
            else if (ImGui::GetIO().KeyShift && m_AssetAnchor.IsValid()) m_RangeTo = handle;
            else m_Ctx.SelectAsset(handle), m_AssetAnchor = handle;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (isDir) m_EnterDir = path;
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
            if (isDir && ImGui::MenuItem("Add to Favorites")) ProjectFavorites::AddFolder(path), m_FavoritesFrame = -1000;
            if (ImGui::MenuItem("Rename")) { m_Renaming = path; m_RenameBuffer = path.filename().string(); m_RenameFocus = true; }
            if (path.extension() == ".prefab" && ImGui::MenuItem("Create Prefab Variant"))
            {
                // A prefab that starts identical to this one and keeps only what it changes.
                AssetHandle base = AssetManager::GetHandle(path);
                AssetHandle variant = Prefabs::CreateVariant(base, UniquePath(path.parent_path() / (path.stem().string() + " Variant.prefab")));
                if (variant.IsValid()) m_Ctx.SelectedAsset = variant;
            }
            if (path.extension() == ".scene" && ImGui::MenuItem("Open Additive", nullptr, false, !m_Ctx.IsPlaying()))
                m_Ctx.RequestOpenAdditive = path;
            if (ImGui::MenuItem("Export Package...")) m_Ctx.RequestExportPackage = path; // with what it references
            if (fs::is_directory(path) && ImGui::MenuItem("Build Asset Bundle"))
            {
                // The folder's files with their .meta (GUIDs) as one pack a running game loads (AssetBundle.LoadFromFile).
                fs::path out = Project::GetDirectory() / "Bundles" / (path.filename().string() + ".pak");
                std::error_code ec;
                fs::create_directories(out.parent_path(), ec);
                if (AssetPack::Write(out, path, AssetPack::ListFiles(path)))
                {
                    Log::Info("Built asset bundle {} ({:.1f} MB)", out.string(), fs::file_size(out, ec) / 1048576.0);
                    FileDialogs::OpenWithDefaultApp(out.parent_path());
                }
                else
                    Log::Error("Building the asset bundle failed");
            }
            if (AssetHandle item = AssetManager::GetHandle(path); item.IsValid())
            {
                ImGui::Separator();
                // The objects in the open scene using it (the Hierarchy's search, dimmed in the Scene view).
                if (ImGui::MenuItem("Find References In Scene", nullptr, false, type != AssetType::Scene))
                    m_Ctx.HierarchySearch = SceneSearch::ReferenceQuery(item);
                // It and everything it uses, in turn.
                if (ImGui::MenuItem("Select Dependencies"))
                {
                    auto deps = AssetDependencies::Collect({ item }, false).Assets;
                    std::vector<AssetHandle> list(deps.begin(), deps.end());
                    std::erase(list, item);
                    std::sort(list.begin(), list.end(), [](AssetHandle a, AssetHandle b) {
                        return AssetManager::GetMetadata(a)->Path < AssetManager::GetMetadata(b)->Path;
                    });
                    list.insert(list.begin(), item);
                    m_Ctx.SelectAssets(list, item);
                }
                // The assets that use it.
                if (ImGui::MenuItem("Select Used By"))
                {
                    auto users = AssetDependencies::UsedBy(item);
                    if (users.empty()) Log::Info("No asset uses {}", path.filename().string());
                    else m_Ctx.SelectAssets(users, users.front());
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Show in Explorer")) FileDialogs::RevealInExplorer(path);
            if (ImGui::MenuItem("Delete")) m_DeletePath = path;
            ImGui::EndPopup();
        }
    }

    void ContentBrowserPanel::DrawRenameField(const fs::path& path, float width)
    {
        ImGui::SetNextItemWidth(width);
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

    void ContentBrowserPanel::SetListView(bool list, bool remember)
    {
        m_ListView = list;
        if (!remember) return;
        EditorPreferences::Get().ProjectListView = list;
        EditorPreferences::Get().Save();
    }

    void ContentBrowserPanel::DrawFavorites()
    {
        // Read again now and then (and at once after a change here).
        if (ImGui::GetFrameCount() - m_FavoritesFrame > 120)
        {
            m_Favorites = ProjectFavorites::List();
            m_FavoritesFrame = ImGui::GetFrameCount();
        }
        const fs::path& root = AssetManager::GetAssetsDirectory();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!ImGui::TreeNodeEx("Favorites", flags)) return;
        std::optional<Favorite> remove;
        for (size_t i = 0; i < m_Favorites.size(); ++i)
        {
            const Favorite& f = m_Favorites[i];
            ImGui::PushID((int)i);
            bool current = f.IsSearch() ? m_Search == f.Search : m_Search.empty() && m_Current == (root / f.Folder).lexically_normal();
            if (m_RenamingFavorite == (int)i)
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (m_RenameFocus) ImGui::SetKeyboardFocusHere(), m_RenameFocus = false;
                if (ImGui::InputText("##favorite", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                {
                    ProjectFavorites::Rename(f, m_RenameBuffer);
                    m_RenamingFavorite = -1;
                    m_FavoritesFrame = -1000;
                }
                else if (ImGui::IsItemDeactivated()) m_RenamingFavorite = -1;
            }
            else
            {
                // A magnifier for searches, a folder mark for folders.
                ImGui::TreeNodeEx("##fav", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                               (current ? ImGuiTreeNodeFlags_Selected : 0),
                                  "%s %s", f.IsSearch() ? "?" : "*", f.Name.c_str());
                if (ImGui::IsItemClicked())
                {
                    if (f.IsSearch()) m_Search = f.Search;
                    else m_Current = (root / f.Folder).lexically_normal(), m_Search.clear();
                }
                ImGui::SetItemTooltip("%s", f.IsSearch() ? ("Search: " + f.Search).c_str() : ("Assets/" + f.Folder).c_str());
                if (ImGui::BeginPopupContextItem("FavoriteContext"))
                {
                    if (ImGui::MenuItem("Rename")) m_RenamingFavorite = (int)i, m_RenameBuffer = f.Name, m_RenameFocus = true;
                    if (ImGui::MenuItem("Remove from Favorites")) remove = f;
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
        if (m_Favorites.empty()) ImGui::TextDisabled("Folders (right-click) and saved searches");
        ImGui::TreePop();
        if (remove)
        {
            ProjectFavorites::Remove(*remove);
            m_FavoritesFrame = -1000;
        }
    }

    void ContentBrowserPanel::DrawList(const std::vector<fs::directory_entry>& found, std::vector<AssetHandle>& shown)
    {
        // Name, type, size and date modified; a header click sorts by that column (folders stay first).
        ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("List", 4, flags, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y - 4.0f, 60.0f)))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 3.0f);
        ImGui::TableSetupColumn("Type", 0, 1.2f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_PreferSortDescending, 1.4f);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0)
        {
            m_SortBy = (AssetSort)specs->Specs[0].ColumnIndex;
            m_SortAscending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        }
        std::vector<AssetListEntry> rows;
        for (const auto& e : found) rows.push_back(AssetListing::Describe(e.path()));
        AssetListing::Sort(rows, m_SortBy, m_SortAscending);
        float iconSize = ImGui::GetTextLineHeight();
        for (const AssetListEntry& row : rows)
        {
            ImGui::PushID(row.Path.string().c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (row.Handle.IsValid()) shown.push_back(row.Handle);
            bool selected = m_Ctx.IsAssetSelected(row.Handle);
            // The icon: a folder, or a badge in the asset type's color (the grid's file colors).
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec4 color = row.Folder ? ImVec4(0.85f, 0.72f, 0.35f, 1) : UI::AssetColor(row.Type);
            if (row.Folder)
            {
                dl->AddRectFilled(ImVec2(at.x + 1, at.y + 2), ImVec2(at.x + iconSize * 0.5f, at.y + 5), ImGui::GetColorU32(color), 1.5f);
                dl->AddRectFilled(ImVec2(at.x + 1, at.y + 4), ImVec2(at.x + iconSize - 1, at.y + iconSize - 2), ImGui::GetColorU32(color), 2.0f);
            }
            else
            {
                ImVec2 a(at.x + 2, at.y + 1), b(at.x + iconSize - 2, at.y + iconSize - 1);
                dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(color.x * 0.45f, color.y * 0.45f, color.z * 0.45f, 1)), 2.0f);
                dl->AddRect(a, b, ImGui::GetColorU32(color), 2.0f, 0, 1.5f);
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + iconSize + 4.0f, at.y));
            if (m_Renaming == row.Path) DrawRenameField(row.Path, -FLT_MIN);
            else
            {
                // Selected rows in the grid's selection color (the theme's Header is a grey).
                if (selected) ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                ImGui::Selectable(row.Name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                if (selected) ImGui::PopStyleColor();
                ItemBehavior(row.Path, row.Name, row.Folder, row.Handle, row.Type, ImGui::IsItemHovered());
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", AssetListing::TypeName(row).c_str());
            ImGui::TableNextColumn();
            if (!row.Folder) ImGui::TextDisabled("%s", AssetListing::FormatSize(row.Size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", AssetListing::FormatTime(row.Modified).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
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
            DrawFavorites();
            DrawFolderTree(root);
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            // Toolbar: back, breadcrumb, search, size
            float rowEnd = ImGui::GetContentRegionAvail().x; // this column's width (SameLine positions are from its start)
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
            // Right-aligned: search, Save, Grid | List, and the thumbnail size in the grid.
            auto buttonWidth = [](const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f; };
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float tools = 180.0f + spacing + buttonWidth("Save") + spacing + buttonWidth("Grid") + 2.0f + buttonWidth("List") +
                          (m_ListView ? 0.0f : spacing + 120.0f);
            ImGui::SameLine(rowEnd - tools);
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##search", "Search (t:Type l:Label)", &m_Search);
            ImGui::SameLine();
            ImGui::BeginDisabled(m_Search.find_first_not_of(' ') == std::string::npos);
            if (ImGui::Button("Save")) ProjectFavorites::AddSearch(m_Search), m_FavoritesFrame = -1000;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Save this search to Favorites (top of the folder tree)");
            // Grid of thumbnails, or a list with columns (remembered in the Preferences).
            ImGui::SameLine();
            if (ToggleButton("Grid", !m_ListView) && m_ListView) SetListView(false);
            ImGui::SameLine(0, 2);
            if (ToggleButton("List", m_ListView) && !m_ListView) SetListView(true);
            if (!m_ListView)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("##size", &m_ThumbnailSize, 48.0f, 160.0f, "%.0f px");
            }
            ImGui::Separator();

            ImGui::BeginChild("Files");
            std::vector<fs::directory_entry> entries;
            std::error_code ec;
            std::string search = Lower(m_Search);
            if (!search.empty())
            {
                // Across the project: names, and t:Type and l:Label terms.
                for (AssetHandle h : AssetManager::Search(m_Search))
                    if (const AssetMetadata* m = AssetManager::GetMetadata(h)) entries.emplace_back(AssetManager::GetAssetsDirectory() / m->Path, ec);
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
            m_DeletePath.clear();
            m_EnterDir.clear();
            m_RangeTo = AssetHandle::None();
            std::vector<AssetHandle> shown; // the assets shown, in order (for Shift click)
            if (m_ListView) DrawList(entries, shown);
            else if (ImGui::BeginTable("Grid", columns))
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
                    bool selected = m_Ctx.IsAssetSelected(handle);
                    if (handle.IsValid()) shown.push_back(handle);

                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("thumb", size);
                    bool hovered = ImGui::IsItemHovered();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 max(cursor.x + size.x, cursor.y + size.y);
                    if (selected || hovered)
                        dl->AddRectFilled(ImVec2(cursor.x - 3, cursor.y - 3), ImVec2(max.x + 3, max.y + 3),
                                          ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), 4.0f);

                    Ref<Texture2D> tex = type == AssetType::Texture ? AssetManager::GetTexture(handle) : nullptr;
                    if (type == AssetType::SpriteAtlas)
                        if (auto atlas = AssetManager::GetSpriteAtlas(handle)) tex = atlas->Texture; // its page
                    Ref<Material> mat = type == AssetType::Material ? AssetManager::GetMaterial(handle) : nullptr;
                    uint64_t preview = m_Ctx.Previews && AssetPreviews::HasPreview(type) ? m_Ctx.Previews->Get(handle) : 0;
                    if (preview)
                    {
                        // The material on a lit sphere, the model or the prefab, rendered.
                        dl->AddImage(ImTextureRef((ImTextureID)preview), cursor, max);
                    }
                    else if (mat)
                    {
                        // Until its preview is drawn, a swatch: base map (if any) tinted by the base color, as a circle.
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

                    ItemBehavior(path, name, isDir, handle, type, hovered);

                    // Label / rename field
                    if (m_Renaming == path) DrawRenameField(path, m_ThumbnailSize);
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
                if (ImGui::MenuItem("New Animator Override Controller"))
                {
                    // Of the selected controller, when one is selected.
                    AssetHandle base = AssetHandle::None();
                    if (auto* meta = AssetManager::GetMetadata(m_Ctx.SelectedAsset); meta && meta->Type == AssetType::AnimatorController) base = m_Ctx.SelectedAsset;
                    AssetHandle h = AssetManager::CreateAnimatorOverride(UniquePath(m_Current / "New Override.overrideController"), base);
                    if (h.IsValid()) m_Ctx.SelectAsset(h);
                }
                if (ImGui::MenuItem("New Color Lookup Texture"))
                {
                    // A neutral 16-slice strip to grade in an image editor (a screenshot graded alongside it shows the look).
                    fs::path p = UniquePath(m_Current / "New Color Lookup.png");
                    if (PostProcess::WriteNeutralLookup(p, 16))
                    {
                        AssetHandle h = AssetManager::Import(p);
                        if (auto* meta = AssetManager::GetMetadata(h))
                        {
                            nlohmann::json settings = meta->ImportSettings;
                            settings["mips"] = false; // exact texels: no blur between slices
                            AssetManager::SetImportSettings(h, settings);
                        }
                        if (h.IsValid()) m_Ctx.SelectAsset(h);
                    }
                }
                if (ImGui::MenuItem("New UI Style Sheet"))
                {
                    // Classes of component values that UI Style components apply (like UI Toolkit's USS).
                    fs::path p = UniquePath(m_Current / "New UI Style Sheet.uistyle");
                    std::ofstream(p) << R"({
  "classes": {
    "title": { "UI Text": { "Font Size": 40, "Color": [1.0, 1.0, 1.0, 1.0] } },
    "panel": { "UI Image": { "Color": [0.08, 0.1, 0.16, 0.9] } },
    "primary": {
      "UI Image": { "Color": [0.16, 0.45, 0.85, 1.0] },
      "UI Button": { "Hover Color": [1.15, 1.15, 1.15, 1.0], "Fade Duration": 0.15 }
    }
  }
}
)";
                    AssetHandle h = AssetManager::Import(p);
                    if (h.IsValid()) m_Ctx.SelectAsset(h);
                }
                if (ImGui::MenuItem("New Render Texture"))
                {
                    AssetHandle h = AssetManager::CreateRenderTexture(UniquePath(m_Current / "New Render Texture.rendertexture"));
                    if (h.IsValid()) m_Ctx.SelectAsset(h);
                }
                if (ImGui::MenuItem("New Timeline"))
                {
                    AssetHandle h = AssetManager::CreateTimeline(UniquePath(m_Current / "New Timeline.timeline"));
                    if (h.IsValid()) m_Ctx.SelectAsset(h);
                }
                if (ImGui::BeginMenu("New Data Asset", !DataAssets::Classes().empty()))
                {
                    // An asset of each data class the scripts define (C# classes deriving from DataAsset).
                    for (const auto& c : DataAssets::Classes())
                        if (ImGui::MenuItem(c.Name.c_str()))
                        {
                            std::string shortName = c.Name.substr(c.Name.find_last_of('.') + 1);
                            AssetHandle h = DataAssets::Create(UniquePath(m_Current / ("New " + shortName + ".asset")), c.Name);
                            if (h.IsValid()) m_Ctx.SelectAsset(h);
                        }
                    ImGui::EndMenu();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && DataAssets::Classes().empty())
                    ImGui::SetTooltip("Derive a C# class from DataAsset to make assets of it");
                if (ImGui::MenuItem("New Sprite Atlas"))
                {
                    // Packs the textures in this folder to begin with.
                    std::vector<AssetHandle> sprites;
                    for (const auto& entry : fs::directory_iterator(m_Current, ec))
                        if (AssetHandle t = AssetManager::GetHandle(entry.path()); t.IsValid())
                            if (const AssetMetadata* m = AssetManager::GetMetadata(t); m && m->Type == AssetType::Texture) sprites.push_back(t);
                    AssetHandle h = AssetManager::CreateSpriteAtlas(UniquePath(m_Current / "New Sprite Atlas.spriteatlas"), sprites);
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
                if (ImGui::MenuItem("Import Package...")) m_Ctx.RequestImportPackage = true;
                if (ImGui::MenuItem("Export Folder as Package...")) m_Ctx.RequestExportPackage = m_Current;
                if (ImGui::MenuItem("Show in Explorer")) FileDialogs::OpenWithDefaultApp(m_Current);
                if (ImGui::MenuItem("Refresh")) AssetManager::Refresh();
                ImGui::EndPopup();
            }
            ImGui::EndChild();
            ImGui::EndTable();

            if (!m_EnterDir.empty()) m_Current = m_EnterDir, m_Search.clear();
            // Shift click: the assets shown from the last clicked one to this one.
            if (m_RangeTo.IsValid())
            {
                auto a = std::find(shown.begin(), shown.end(), m_AssetAnchor), b = std::find(shown.begin(), shown.end(), m_RangeTo);
                if (a != shown.end() && b != shown.end())
                    m_Ctx.SelectAssets(std::vector<AssetHandle>(std::min(a, b), std::max(a, b) + 1), m_RangeTo);
                else
                    m_Ctx.SelectAsset(m_RangeTo), m_AssetAnchor = m_RangeTo;
            }
            if (!m_DeletePath.empty() && FileDialogs::Confirm("Delete Asset", "Delete '" + m_DeletePath.filename().string() + "'? This cannot be undone."))
            {
                if (m_Ctx.IsAssetSelected(AssetManager::GetHandle(m_DeletePath))) m_Ctx.SelectAsset(AssetHandle::None());
                AssetManager::DeleteAsset(m_DeletePath);
            }
        }
        ImGui::End();
    }
}
