#include "Panels.h"
#include "../EditorActions.h"
#include "../EditorUI.h"
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scene/SceneSearch.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace Editor
{
    void HierarchyPanel::OnImGui(bool* open)
    {
        if (!ImGui::Begin("Hierarchy", open))
        {
            ImGui::End();
            return;
        }
        Scene& scene = m_Ctx.ActiveScene();

        // Search: a flat list of the matching objects instead of the tree.
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search (name, t:Type, tag:Tag)", &m_Ctx.HierarchySearch);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Words must all match: a plain word is part of the name, t:Rigidbody (or t:MeshRenderer,\n"
                              "or a script class) a component the object has, tag:Player its tag. Esc clears.");
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) m_Ctx.HierarchySearch.clear();
        if (!m_Ctx.HierarchySearch.empty())
        {
            DrawSearchResults(scene);
            ImGui::End();
            return;
        }

        // Scene header
        std::string title = scene.GetName() + (m_Ctx.Dirty ? "*" : "");
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        bool sceneOpen = ImGui::TreeNodeEx("##scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", title.c_str());
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kEntityPayload))
            {
                m_PendingChild = scene.FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                m_PendingMode = 3;
            }
            ImGui::EndDragDropTarget();
        }
        m_LastVisibleOrder = std::move(m_VisibleOrder);
        m_VisibleOrder.clear();
        if (sceneOpen)
        {
            // Copy: nodes may be reordered by drag-drop during iteration (applied after the loop anyway).
            auto roots = scene.GetRootEntities();
            for (Entity e : roots) DrawNode(scene, e);
            ImGui::TreePop();
        }

        // Empty area: deselect, context menu, drop to root.
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 40.0f)));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kEntityPayload))
            {
                m_PendingChild = scene.FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                m_PendingMode = 3;
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kAssetPayload))
            {
                AssetHandle h(*(const uint64_t*)p->Data);
                if (auto* meta = AssetManager::GetMetadata(h); meta && meta->Type == AssetType::Prefab)
                {
                    m_Ctx.RecordUndo();
                    m_Ctx.Select(scene.InstantiatePrefab(h, glm::vec3(0.0f)));
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) m_Ctx.Select(Entity::Null());
        if (ImGui::BeginPopupContextWindow("HierarchyContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            DrawCreateMenuItems(m_Ctx, Entity::Null());
            ImGui::EndPopup();
        }

        // Apply drag-drop structural edits after drawing.
        if (m_PendingMode >= 0 && scene.IsValid(m_PendingChild))
        {
            m_Ctx.RecordUndo();
            switch (m_PendingMode)
            {
            case 0: scene.SetParent(m_PendingChild, m_PendingParent); break;
            case 3: scene.SetParent(m_PendingChild, Entity::Null()); break;
            case 1:
            case 2:
            {
                Entity target = m_PendingParent;
                Entity newParent = scene.GetParent(target);
                scene.SetParent(m_PendingChild, newParent);
                const auto& siblings = newParent.IsNull() ? scene.GetRootEntities() : scene.GetChildren(newParent);
                size_t index = (size_t)(std::find(siblings.begin(), siblings.end(), target) - siblings.begin());
                // SetSiblingIndex removes the child first; account for its current position.
                size_t childIndex = (size_t)(std::find(siblings.begin(), siblings.end(), m_PendingChild) - siblings.begin());
                if (childIndex < index) --index;
                scene.SetSiblingIndex(m_PendingChild, m_PendingMode == 1 ? index : index + 1);
                break;
            }
            }
        }
        m_PendingMode = -1;

        // Keyboard shortcuts while the panel is focused.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && m_Renaming.IsNull() && m_Ctx.HasValidSelection())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) BeginRename(m_Ctx.Selected);
            if (ImGui::IsKeyPressed(ImGuiKey_F)) m_Ctx.RequestFocusSelected = true;
        }
        ImGui::End();
    }

    void HierarchyPanel::DrawSearchResults(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        std::vector<Entity> found = SceneSearch::Find(scene, m_Ctx.HierarchySearch);
        ImGui::TextDisabled("%zu found", found.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select All"))
        {
            m_Ctx.Select(Entity::Null());
            for (Entity e : found) m_Ctx.AddToSelection(e);
        }
        ImGui::Separator();
        ImGui::BeginChild("##results");
        for (Entity e : found)
        {
            ImGui::PushID((int)e.Index);
            bool active = reg.Get<TransformComponent>(e).ActiveInHierarchy;
            if (!active) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(reg.Get<TagComponent>(e).Name.c_str(), m_Ctx.IsSelected(e), ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (ImGui::GetIO().KeyCtrl) m_Ctx.ToggleSelect(e);
                else m_Ctx.Select(e);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_Ctx.RequestFocusSelected = true;
            }
            if (!active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                // Where it is: its parents' names.
                std::string path = reg.Get<TagComponent>(e).Name;
                for (Entity p = scene.GetParent(e); !p.IsNull(); p = scene.GetParent(p)) path = reg.Get<TagComponent>(p).Name + " / " + path;
                ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && m_Ctx.HasValidSelection() && ImGui::IsKeyPressed(ImGuiKey_F) &&
            !ImGui::GetIO().WantTextInput)
            m_Ctx.RequestFocusSelected = true;
    }

    void HierarchyPanel::DrawVisibilityToggles(Scene& scene, Entity e, ImVec2 rowMin, ImVec2 rowMax, bool rowHovered)
    {
        // Scene visibility at the row's right: an eye (shown / hidden in the Scene view) and a pointer (picked by clicks
        // there or not). Shown while the row is hovered or something is set; dim when it comes from a parent.
        SceneVisibility& vis = m_Ctx.Visibility;
        bool hiddenSelf = vis.IsHiddenSelf(scene, e), hidden = hiddenSelf || vis.IsHidden(scene, e);
        bool unpickableSelf = vis.IsUnpickableSelf(scene, e), unpickable = unpickableSelf || vis.IsUnpickable(scene, e);
        if (!rowHovered && !hidden && !unpickable) return;
        float size = rowMax.y - rowMin.y;
        ImVec2 restore = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        auto toggle = [&](int slot, const char* id, bool on, bool self, const char* tip, auto&& drawIcon) {
            ImVec2 at(rowMax.x - size * (float)(slot + 1) - 4.0f, rowMin.y);
            ImGui::SetCursorScreenPos(at);
            bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImU32 color = on && !self ? dim : ImGui::IsItemHovered() ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : (on ? text : dim);
            ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
            drawIcon(c, size * 0.32f, color);
            if (on) dl->AddLine(ImVec2(c.x - size * 0.32f, c.y + size * 0.32f), ImVec2(c.x + size * 0.32f, c.y - size * 0.32f), color, 1.5f);
            return clicked;
        };
        auto eye = [&](ImVec2 c, float r, ImU32 color) {
            dl->AddEllipse(c, ImVec2(r, r * 0.6f), color, 0.0f, 0, 1.5f);
            dl->AddCircleFilled(c, r * 0.3f, color);
        };
        auto pointer = [&](ImVec2 c, float r, ImU32 color) {
            dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), ImVec2(c.x + r * 0.7f, c.y + r * 0.1f), color);
        };
        if (toggle(1, "##hide", hidden, hiddenSelf, hidden ? "Hidden in the Scene view (click to show; H)" : "Hide in the Scene view (H)", eye))
        {
            vis.SetHidden(scene, e, !hiddenSelf);
            m_Ctx.VisibilityChanged = true;
        }
        if (toggle(0, "##pick", unpickable, unpickableSelf, unpickable ? "Not picked in the Scene view (click to allow)" : "Disable picking in the Scene view", pointer))
        {
            vis.SetPickable(scene, e, unpickableSelf);
            m_Ctx.VisibilityChanged = true;
        }
        ImGui::SetCursorScreenPos(restore);
    }

    void HierarchyPanel::DrawNode(Scene& scene, Entity e)
    {
        auto& reg = scene.GetRegistry();
        auto& tag = reg.Get<TagComponent>(e);
        const auto& children = scene.GetChildren(e);
        bool active = reg.Get<TransformComponent>(e).ActiveInHierarchy;

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_FramePadding;
        if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (m_Ctx.IsSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
        m_VisibleOrder.push_back(e);
        // Auto-expand ancestors of the selection.
        if (!m_Ctx.Selected.IsNull() && scene.IsValid(m_Ctx.Selected) && scene.IsDescendantOf(m_Ctx.Selected, e))
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        ImGui::PushID((int)e.Index);
        const auto* prefabLink = reg.TryGet<PrefabLinkComponent>(e);
        int pushedColors = 0;
        if (prefabLink)
        {
            // Prefab instances are blue like in Unity (dimmer when inactive).
            ImVec4 prefabText = UI::AccentText();
            if (!active) prefabText.w = 0.55f;
            ImGui::PushStyleColor(ImGuiCol_Text, prefabText);
            ++pushedColors;
        }
        else if (!active)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ++pushedColors;
        }
        bool opened;
        if (m_Renaming == e)
        {
            opened = ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_AllowOverlap, "");
            ImGui::SameLine();
            if (m_RenameFocus)
            {
                m_RenameBuffer = tag.Name;
                ImGui::SetKeyboardFocusHere();
                m_RenameFocus = false;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                m_Ctx.RecordUndo();
                reg.Get<TagComponent>(e).Name = m_RenameBuffer;
                Prefabs::RecordFieldOverride(scene, e, "Tag", "Name");
                m_Renaming = Entity::Null();
            }
            else if (ImGui::IsItemDeactivated()) m_Renaming = Entity::Null();
        }
        else
            opened = ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", tag.Name.c_str());
        ImGui::PopStyleColor(pushedColors);
        ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
        bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && !m_Anchor.IsNull())
            {
                // Range between the anchor and this node in the order drawn last frame.
                auto a = std::find(m_LastVisibleOrder.begin(), m_LastVisibleOrder.end(), m_Anchor);
                auto b = std::find(m_LastVisibleOrder.begin(), m_LastVisibleOrder.end(), e);
                if (a != m_LastVisibleOrder.end() && b != m_LastVisibleOrder.end())
                {
                    if (a > b) std::swap(a, b);
                    if (!io.KeyCtrl) m_Ctx.Select(Entity::Null());
                    for (auto it = a; it <= b; ++it) m_Ctx.AddToSelection(*it);
                    m_Ctx.AddToSelection(e);
                }
            }
            else if (io.KeyCtrl)
            {
                m_Ctx.ToggleSelect(e);
                m_Anchor = e;
            }
            else
            {
                m_Ctx.Select(e);
                m_Anchor = e;
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_Ctx.Select(e);
            m_Ctx.RequestFocusSelected = true;
        }

        if (ImGui::BeginDragDropSource())
        {
            uint64_t guid = scene.GetGuid(e);
            ImGui::SetDragDropPayload(UI::kEntityPayload, &guid, sizeof(guid));
            ImGui::Text("%s", tag.Name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            // Top quarter: insert before, bottom quarter: after, middle: make child.
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            float y = (ImGui::GetMousePos().y - min.y) / std::max(1.0f, max.y - min.y);
            int mode = y < 0.25f ? 1 : y > 0.75f ? 2 : 0;
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kEntityPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery))
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 col = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
                if (mode == 0) dl->AddRect(min, max, col, 2.0f, 0, 2.0f);
                else dl->AddLine(ImVec2(min.x, mode == 1 ? min.y : max.y), ImVec2(max.x, mode == 1 ? min.y : max.y), col, 2.0f);
                if (p->IsDelivery())
                {
                    Entity dragged = scene.FindEntityByGuid(Guid(*(const uint64_t*)p->Data));
                    if (dragged != e && !scene.IsDescendantOf(e, dragged))
                    {
                        m_PendingChild = dragged;
                        m_PendingParent = e;
                        m_PendingMode = mode;
                    }
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kAssetPayload))
            {
                AssetHandle h(*(const uint64_t*)p->Data);
                if (auto* meta = AssetManager::GetMetadata(h); meta && meta->Type == AssetType::Prefab)
                {
                    m_Ctx.RecordUndo();
                    m_Ctx.Select(scene.InstantiatePrefab(h, glm::vec3(0.0f), e));
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem("EntityContext"))
        {
            if (!m_Ctx.IsSelected(e)) m_Ctx.Select(e);
            if (ImGui::MenuItem("Rename", "F2")) BeginRename(e);
            if (ImGui::MenuItem("Copy", "Ctrl+C")) CopySelected(m_Ctx);
            if (ImGui::MenuItem("Paste", "Ctrl+V")) PasteClipboard(m_Ctx);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) DuplicateSelected(m_Ctx);
            if (ImGui::MenuItem("Delete", "Del")) DeleteSelected(m_Ctx);
            ImGui::Separator();
            if (ImGui::MenuItem("Create Prefab")) CreatePrefab(m_Ctx, e, AssetManager::GetAssetsDirectory());
            Entity instanceRoot = Prefabs::FindInstanceRoot(scene, e);
            if (!instanceRoot.IsNull())
            {
                if (ImGui::MenuItem("Select Prefab Asset"))
                    m_Ctx.SelectAsset(scene.GetRegistry().Get<PrefabLinkComponent>(instanceRoot).Prefab);
                if (ImGui::MenuItem("Unpack Prefab"))
                {
                    m_Ctx.RecordUndo();
                    Prefabs::Unpack(scene, instanceRoot);
                }
            }
            if (ImGui::MenuItem("Frame Selected", "F")) m_Ctx.RequestFocusSelected = true;
            ImGui::Separator();
            if (ImGui::BeginMenu("Create Child")) { DrawCreateMenuItems(m_Ctx, e); ImGui::EndMenu(); }
            ImGui::EndPopup();
        }
        DrawVisibilityToggles(scene, e, rowMin, rowMax, rowHovered);

        if (opened)
        {
            if (scene.IsValid(e))
            {
                auto kids = scene.GetChildren(e); // copy
                for (Entity c : kids)
                    if (scene.IsValid(c)) DrawNode(scene, c);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
