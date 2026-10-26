#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Scene/ComponentRegistry.h>
#include <Dimension/Scripting/ScriptRegistry.h>
#include <Dimension/Scripting/ScriptableEntity.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Audio/AudioEngine.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Audio/AudioMixer.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Animation/AnimatorController.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <map>

namespace Editor
{
    namespace
    {
        // [RequireComponent]: components a script needs, added (if missing) when the script is added in the editor.
        void AddRequiredComponents(Scene& scene, Entity e, const std::string& scriptName)
        {
            const ScriptInfo* script = ScriptRegistry::Find(scriptName);
            if (!script) return;
            for (const std::string& name : script->Requires)
                if (const ComponentInfo* info = ComponentRegistry::Find(name); info && !info->Has(scene, e))
                {
                    info->Add(scene, e);
                    Prefabs::RecordComponentAdded(scene, e, info->Name);
                }
        }

        // A script's label in menus: its [AddComponentMenu] name, else the class name, with its language.
        std::string ScriptLabel(const ScriptInfo& s)
        {
            std::string name = s.Menu.empty() ? s.Name : s.Menu.substr(s.Menu.find_last_of('/') == std::string::npos ? 0 : s.Menu.find_last_of('/') + 1);
            return name + (s.Module == "csharp" ? "   (C#)" : "   (C++)");
        }
        // Undo/dirty bookkeeping for a property edit. Widgets such as checkboxes change
        // their value on the same frame they activate, so the pre-edit value is restored
        // briefly while the undo snapshot is captured.
        template <class T>
        void ApplyEdit(EditorContext& ctx, const UI::EditResult& r, T& value, const T& before)
        {
            if (r.Activated)
            {
                if (r.Changed)
                {
                    T after = value;
                    value = before;
                    ctx.RecordUndo();
                    value = after;
                }
                else ctx.RecordUndo();
            }
            if (r.Changed) ctx.MarkDirty();
        }

        // Same, for a reflected field (values round-trip through JSON).
        void ApplyFieldEdit(EditorContext& ctx, const UI::EditResult& r, const FieldInfo& f, void* object,
                            const nlohmann::json& before, const EntityRefResolver& resolver)
        {
            if (r.Activated)
            {
                if (r.Changed)
                {
                    nlohmann::json after = FieldToJson(f, object, &resolver);
                    FieldFromJson(f, object, before, &resolver);
                    ctx.RecordUndo();
                    FieldFromJson(f, object, after, &resolver);
                }
                else ctx.RecordUndo();
            }
            if (r.Changed) ctx.MarkDirty();
        }

        EntityRefResolver MakeResolver(Scene& scene)
        {
            EntityRefResolver resolver;
            resolver.ToGuid = [&scene](Entity x) { return scene.IsValid(x) ? (uint64_t)scene.GetGuid(x) : uint64_t(0); };
            resolver.FromGuid = [&scene](uint64_t g) { return scene.FindEntityByGuid(Guid(g)); };
            return resolver;
        }

        enum class ComponentAction { None, Reset, Remove, Copy, PasteValues, PasteNew, MoveUp, MoveDown };

        // Component values travel through the OS clipboard as JSON, so they can be pasted into
        // another object, scene, or editor instance (Unity's Copy Component / Paste Component Values).
        constexpr const char* kClipboardKey = "DimensionComponent";

        bool ReadComponentClipboard(std::string& name, nlohmann::json& fields)
        {
            const char* text = ImGui::GetClipboardText();
            if (!text || !*text || text[0] != '{') return false;
            auto j = nlohmann::json::parse(text, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains(kClipboardKey) || !j[kClipboardKey].is_string()) return false;
            name = j[kClipboardKey].get<std::string>();
            fields = j.value("fields", nlohmann::json::object());
            return true;
        }

        // Draws a component's header; returns whether it is expanded. `action` reports the menu choice.
        bool ComponentHeader(const ComponentInfo& info, ComponentAction& action, bool canMoveUp, bool canMoveDown)
        {
            const char* name = info.Name.c_str();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 5));
            bool open = ImGui::TreeNodeEx(name, flags, "%s", name);
            ImGui::PopStyleVar();
            bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right); // right-click the header, or use "..."
            float w = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
            ImGui::PushID(name);
            if (ImGui::Button("...", ImVec2(w, w)) || rightClicked) ImGui::OpenPopup("ComponentSettings");
            if (ImGui::BeginPopup("ComponentSettings"))
            {
                bool copyable = !info.Fields.empty(); // scripts use their own serialization
                std::string clipName;
                nlohmann::json clipFields;
                bool hasClip = ReadComponentClipboard(clipName, clipFields);
                if (ImGui::MenuItem("Reset")) action = ComponentAction::Reset;
                ImGui::Separator();
                if (ImGui::MenuItem("Copy Component", nullptr, false, copyable)) action = ComponentAction::Copy;
                std::string pasteValues = hasClip ? "Paste Component Values (" + clipName + ")" : "Paste Component Values";
                if (ImGui::MenuItem(pasteValues.c_str(), nullptr, false, hasClip && clipName == info.Name)) action = ComponentAction::PasteValues;
                std::string pasteNew = hasClip ? "Paste Component As New (" + clipName + ")" : "Paste Component As New";
                if (ImGui::MenuItem(pasteNew.c_str(), nullptr, false, hasClip && clipName != info.Name)) action = ComponentAction::PasteNew;
                ImGui::Separator();
                if (ImGui::MenuItem("Remove Component", nullptr, false, info.Removable)) action = ComponentAction::Remove;
                ImGui::Separator();
                if (ImGui::MenuItem("Move Up", nullptr, false, canMoveUp)) action = ComponentAction::MoveUp;
                if (ImGui::MenuItem("Move Down", nullptr, false, canMoveDown)) action = ComponentAction::MoveDown;
                ImGui::EndPopup();
            }
            ImGui::PopID();
            return open;
        }
    }

    void InspectorPanel::OnImGui(bool* open)
    {
        if (!ImGui::Begin("Inspector", open))
        {
            ImGui::End();
            return;
        }
        Scene& scene = m_Ctx.ActiveScene();
        if (m_Ctx.HasValidSelection())
        {
            size_t count = 0;
            for (Entity s : m_Ctx.Selection)
                if (scene.IsValid(s)) ++count;
            if (count > 1)
            {
                ImGui::TextColored(UI::AccentText(), "%zu objects selected", count);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("Showing components they all have. Values that differ show as --; edits apply to all.");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }
            DrawEntity(scene, m_Ctx.Selected);
        }
        else if (m_Ctx.SelectedAsset.IsValid()) DrawAsset(m_Ctx.SelectedAsset);
        else ImGui::TextDisabled("Select a GameObject or asset to inspect it.");
        ImGui::End();
    }

    void InspectorPanel::DrawEntity(Scene& scene, Entity e)
    {
        auto& reg = scene.GetRegistry();
        ImGui::PushID((int)e.Index);

        // Header: active toggle, name, tag
        {
            auto& tag = reg.Get<TagComponent>(e);
            TagComponent before = tag;
            // Multi-selection: the header edits every selected object; values that differ show as "--".
            std::vector<Entity> others;
            for (Entity s : m_Ctx.Selection)
                if (s != e && scene.IsValid(s)) others.push_back(s);
            bool mixActive = false, mixName = false, mixTag = false, mixLayer = false;
            for (Entity o : others)
            {
                const auto& ot = reg.Get<TagComponent>(o);
                mixActive |= ot.Active != tag.Active;
                mixName |= ot.Name != tag.Name;
                mixTag |= ot.Tag != tag.Tag;
                mixLayer |= ot.Layer != tag.Layer;
            }
            UI::EditResult r;
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixActive);
            r.Changed = ImGui::Checkbox("##active", &tag.Active);
            ImGui::PopItemFlag();
            r.Activated = ImGui::IsItemActivated();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (mixName)
            {
                static std::string typed;
                typed.clear();
                if (ImGui::InputTextWithHint("##name", "--", &typed))
                {
                    tag.Name = typed;
                    r.Changed = true;
                }
            }
            else
                r.Changed |= ImGui::InputText("##name", &tag.Name);
            r.Activated |= ImGui::IsItemActivated();
            ImGui::TextDisabled("Tag");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("##tag", mixTag ? "--" : tag.Tag.c_str()))
            {
                for (const char* t : { "Untagged", "Player", "Enemy", "MainCamera", "Respawn", "Finish", "Pulse" })
                    if (ImGui::Selectable(t, tag.Tag == t))
                    {
                        r.Activated = r.Changed = true;
                        tag.Tag = t;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Layer");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            {
                const auto& layers = Project::GetConfig().PhysicsLayers;
                int current = std::clamp(tag.Layer, 0, ProjectConfig::kMaxLayers - 1);
                std::string preview = mixLayer ? std::string("--") : std::to_string(current) + ": " + (layers[current].empty() ? "(unnamed)" : layers[current]);
                if (ImGui::BeginCombo("##layer", preview.c_str()))
                {
                    for (int i = 0; i < ProjectConfig::kMaxLayers; ++i)
                    {
                        if (layers[i].empty() && i != current) continue;
                        std::string label = std::to_string(i) + ": " + (layers[i].empty() ? "(unnamed)" : layers[i]);
                        if (ImGui::Selectable(label.c_str(), i == current) && i != current)
                        {
                            r.Activated = r.Changed = true;
                            tag.Layer = i;
                        }
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Edit layers in File > Project Settings");
                    ImGui::EndCombo();
                }
            }
            ImGui::TextDisabled("ID %llu", (unsigned long long)(uint64_t)scene.GetGuid(e));
            ApplyEdit(m_Ctx, r, tag, before);
            if (r.Changed)
            {
                for (Entity o : others) // only the property that was edited
                {
                    auto& ot = reg.Get<TagComponent>(o);
                    if (tag.Active != before.Active) ot.Active = tag.Active;
                    if (tag.Name != before.Name) ot.Name = tag.Name;
                    if (tag.Tag != before.Tag) ot.Tag = tag.Tag;
                    if (tag.Layer != before.Layer) ot.Layer = tag.Layer;
                }
                scene.UpdateTransforms();
            }
        }
        ImGui::Separator();
        DrawPrefabHeader(scene, e);
        EntityRefResolver resolver = MakeResolver(scene);

        // With several objects selected, only components they all share are shown (like Unity); the
        // header actions and field edits then apply to every selected object.
        std::vector<Entity> targets;
        for (Entity s : m_Ctx.Selection)
            if (scene.IsValid(s)) targets.push_back(s);
        if (std::find(targets.begin(), targets.end(), e) == targets.end()) targets.push_back(e);

        const ComponentInfo* removeInfo = nullptr;
        const ComponentInfo* resetInfo = nullptr;
        std::string pasteValuesInto;
        bool pasteNew = false;
        const ComponentInfo* moveInfo = nullptr;
        int moveDelta = 0;
        auto ordered = ComponentRegistry::GetDisplayOrder(scene, e);
        for (size_t oi = 0; oi < ordered.size(); ++oi)
        {
            const ComponentInfo& info = *ordered[oi];
            if (!std::all_of(targets.begin(), targets.end(), [&](Entity t) { return info.Has(scene, t); })) continue;
            ComponentAction action = ComponentAction::None;
            bool canUp = info.Removable && oi > 0 && ordered[oi - 1]->Removable;
            bool canDown = info.Removable && oi + 1 < ordered.size();
            bool open = ComponentHeader(info, action, canUp, canDown);
            if (action == ComponentAction::MoveUp || action == ComponentAction::MoveDown)
            {
                moveInfo = &info;
                moveDelta = action == ComponentAction::MoveUp ? -1 : 1;
            }
            bool remove = action == ComponentAction::Remove, reset = action == ComponentAction::Reset;
            if (action == ComponentAction::Copy)
            {
                nlohmann::json fields = nlohmann::json::object();
                void* data = info.Get(scene, e);
                for (auto& f : info.Fields) fields[f.Name] = FieldToJson(f, data, &resolver);
                nlohmann::json clip{ { kClipboardKey, info.Name }, { "fields", fields } };
                ImGui::SetClipboardText(clip.dump(2).c_str());
                Log::Info("Copied component '{}'", info.Name);
            }
            if (action == ComponentAction::PasteValues) pasteValuesInto = info.Name;
            if (action == ComponentAction::PasteNew) pasteNew = true;
            if (open)
            {
                if (info.TypeId == GetComponentTypeId<ScriptComponent>())
                    DrawScript(scene, e);
                else if (UI::BeginProperties(info.Name.c_str()))
                {
                    void* data = info.Get(scene, e);
                    for (auto& f : info.Fields)
                    {
                        if (f.Hidden) continue;
                        nlohmann::json before = FieldToJson(f, data, &resolver);
                        UI::EditResult r;
                        bool handled = false;
                        if (info.TypeId == GetComponentTypeId<AnimatorComponent>() && f.Name == "Clip")
                        {
                            // Offer the clips of the entity's skinned mesh (or of its Animation Source).
                            auto* mr = reg.TryGet<MeshRendererComponent>(e);
                            AssetHandle source = static_cast<AnimatorComponent*>(data)->ClipSource;
                            auto mesh = source.IsValid() ? AssetManager::GetMesh(source) : mr ? AssetManager::GetMesh(mr->Mesh) : nullptr;
                            if (mesh && !mesh->GetClips().empty())
                            {
                                auto& anim = *static_cast<AnimatorComponent*>(data);
                                UI::PropertyLabel("Clip");
                                const char* preview = anim.Clip.empty() ? mesh->GetClips().front().Name.c_str() : anim.Clip.c_str();
                                if (ImGui::BeginCombo("##clip", preview))
                                {
                                    for (auto& c : mesh->GetClips())
                                        if (ImGui::Selectable(c.Name.c_str(), c.Name == anim.Clip) && c.Name != anim.Clip)
                                        {
                                            r.Activated = r.Changed = true;
                                            anim.Clip = c.Name;
                                            anim.Time = 0.0f;
                                        }
                                    ImGui::EndCombo();
                                }
                                handled = true;
                            }
                        }
                        if (info.TypeId == GetComponentTypeId<AudioSourceComponent>() && f.Name == "Output")
                        {
                            auto& src = *static_cast<AudioSourceComponent*>(data);
                            UI::PropertyLabel("Output", f.Tooltip.c_str());
                            if (ImGui::BeginCombo("##output", src.Output.empty() ? "Master" : src.Output.c_str()))
                            {
                                for (auto& g : AudioMixer::Groups())
                                {
                                    bool isMaster = &g == &AudioMixer::Groups().front();
                                    bool selected = isMaster ? src.Output.empty() || src.Output == g.Name : src.Output == g.Name;
                                    if (ImGui::Selectable(g.Name.c_str(), selected))
                                    {
                                        r.Activated = r.Changed = true;
                                        src.Output = isMaster ? std::string() : g.Name;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if (!src.Output.empty() && AudioMixer::FindGroup(src.Output) < 0)
                                ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Unknown group: plays through Master");
                            handled = true;
                        }
                        bool overridden = Prefabs::IsOverridden(scene, e, info.Name, f.Name);
                        if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, UI::AccentText());
                        if (!handled)
                        {
                            uint32_t mixed = 0;
                            for (Entity t : targets)
                                if (t != e)
                                    if (void* td = info.Get(scene, t)) mixed |= UI::MixedMask(f.Type, before, FieldToJson(f, td, &resolver));
                            UI::SetMixedValue(mixed);
                            r = UI::Field(f, data, &scene);
                        }
                        if (overridden) ImGui::PopStyleColor();
                        if (r.Changed)
                            for (Entity target : m_Ctx.Selection)
                                if (scene.IsValid(target) && info.Has(scene, target)) Prefabs::RecordFieldOverride(scene, target, info.Name, f.Name);
                        if (r.Changed && !m_Ctx.IsSelected(e)) Prefabs::RecordFieldOverride(scene, e, info.Name, f.Name);
                        ApplyFieldEdit(m_Ctx, r, f, data, before, resolver);
                        if (r.Changed)
                        {
                            // Multi-edit: copy the new value to the same field on every other selected object. Vectors
                            // copy only the components that changed (dragging X keeps each object's own Y and Z), and
                            // lists only the elements that changed, onto objects whose list has the same length.
                            nlohmann::json value = FieldToJson(f, data, &resolver);
                            bool perComponent = (UI::IsVectorField(f.Type) || UI::IsListField(f.Type)) && before.is_array() && value.is_array() &&
                                                before.size() == value.size();
                            for (Entity other : m_Ctx.Selection)
                                if (other != e && scene.IsValid(other))
                                    if (void* od = info.Get(scene, other))
                                    {
                                        nlohmann::json merged = value;
                                        if (perComponent)
                                        {
                                            nlohmann::json own = FieldToJson(f, od, &resolver);
                                            if (own.is_array() && own.size() == value.size())
                                            {
                                                merged = own;
                                                for (size_t i = 0; i < value.size(); ++i)
                                                    if (before[i] != value[i]) merged[i] = value[i];
                                            }
                                        }
                                        FieldFromJson(f, od, merged, &resolver);
                                    }
                            data = info.Get(scene, e);
                            scene.UpdateTransforms();
                        }
                    }
                    UI::EndProperties();
                    if (info.TypeId == GetComponentTypeId<ReflectionProbeComponent>())
                    {
                        if (ImGui::Button("Bake Reflection Probes")) SceneRenderer::RequestProbeBake();
                        ImGui::SameLine();
                        UI::HelpMarker("Probes capture when first shown, when moved or resized, and when play starts. "
                                       "Bake again after changing what is around them (or enable Realtime).");
                    }
                    if (info.TypeId == GetComponentTypeId<MeshRendererComponent>())
                    {
                        auto& mr = *static_cast<MeshRendererComponent*>(info.Get(scene, e));
                        size_t slots = AssetManager::GetMeshMaterials(mr.Mesh).size();
                        if (slots > 0 && !AssetManager::GetMaterial(mr.Material))
                            ImGui::TextWrapped("Using %zu material(s) from the model. Color, Albedo, Smoothness and Emission only "
                                               "apply to parts without one; assign a Material to override all parts.", slots);
                        // Slot count of the mesh: the model's material slots, or one per sub-mesh.
                        size_t meshSlots = slots;
                        if (Ref<Mesh> mesh = AssetManager::GetMesh(mr.Mesh)) meshSlots = std::max(meshSlots, mesh->GetSubMeshes().size());
                        if (meshSlots > 1 && mr.Materials.size() != meshSlots)
                        {
                            if (ImGui::Button("Match Mesh Slots"))
                            {
                                m_Ctx.RecordUndo();
                                for (Entity t : targets)
                                    if (auto* other = scene.GetRegistry().TryGet<MeshRendererComponent>(t))
                                        other->Materials.resize(meshSlots, AssetHandle::None());
                                m_Ctx.MarkDirty();
                            }
                            if (ImGui::IsItemHovered())
                            {
                                std::string tip = "Sizes Materials to the mesh's material slots so each part can take its own override.";
                                auto names = AssetManager::GetMeshMaterialNames(mr.Mesh);
                                for (size_t n = 0; n < names.size(); ++n) tip += "\nSlot " + std::to_string(n) + ": " + names[n];
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                        }
                    }
                }
                ImGui::TreePop();
            }
            if (reset) resetInfo = &info;
            if (remove) removeInfo = &info;
            ImGui::Spacing();
        }
        // Structural changes after the loop (they invalidate component pointers), applied to every target.
        if (moveInfo)
        {
            m_Ctx.RecordUndo();
            for (Entity t : targets)
                if (ComponentRegistry::MoveComponent(scene, t, moveInfo->Name, moveDelta))
                    Prefabs::RecordFieldOverride(scene, t, "Tag", "Component Order");
            m_Ctx.MarkDirty();
        }
        if (resetInfo)
        {
            m_Ctx.RecordUndo();
            for (Entity t : targets)
            {
                resetInfo->Remove(scene, t);
                resetInfo->Add(scene, t);
                for (auto& f : resetInfo->Fields) Prefabs::RecordFieldOverride(scene, t, resetInfo->Name, f.Name);
            }
        }
        if (removeInfo)
        {
            m_Ctx.RecordUndo();
            for (Entity t : targets)
            {
                removeInfo->Remove(scene, t);
                Prefabs::RecordComponentRemoved(scene, t, removeInfo->Name);
            }
        }
        if (!pasteValuesInto.empty() || pasteNew)
        {
            std::string clipName;
            nlohmann::json clipFields;
            const ComponentInfo* clipInfo = ReadComponentClipboard(clipName, clipFields) ? ComponentRegistry::Find(clipName) : nullptr;
            if (clipInfo)
            {
                m_Ctx.RecordUndo();
                for (Entity t : targets)
                {
                    if (pasteNew && !clipInfo->Has(scene, t)) clipInfo->Add(scene, t);
                    if (!clipInfo->Has(scene, t)) continue;
                    void* data = clipInfo->Get(scene, t);
                    for (auto& f : clipInfo->Fields)
                        if (clipFields.contains(f.Name))
                        {
                            FieldFromJson(f, data, clipFields[f.Name], &resolver);
                            Prefabs::RecordFieldOverride(scene, t, clipInfo->Name, f.Name);
                        }
                }
                scene.UpdateTransforms();
                Log::Info("Pasted component '{}' into {} object(s)", clipName, targets.size());
            }
        }

        ImGui::Separator();
        DrawAddComponent(scene, e);
        ImGui::PopID();
    }

    void InspectorPanel::DrawPrefabHeader(Scene& scene, Entity e)
    {
        // Part of a prefab nested inside this one: changes here are overrides of it; opening it edits it everywhere.
        if (auto* l = scene.GetRegistry().TryGet<PrefabLinkComponent>(e); l && l->NestedPrefab.IsValid() && AssetManager::GetMetadata(l->NestedPrefab))
        {
            ImGui::TextDisabled("Nested prefab:");
            ImGui::SameLine();
            ImGui::TextUnformatted(AssetManager::GetDisplayName(l->NestedPrefab).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open##nested")) m_Ctx.RequestOpenPrefab = l->NestedPrefab;
            ImGui::SameLine();
            ImGui::BeginDisabled(m_Ctx.IsPlaying());
            if (ImGui::SmallButton("Apply to Nested"))
            {
                m_Ctx.RecordUndo();
                Prefabs::ApplyToNested(scene, e);
            }
            ImGui::SetItemTooltip("Write this nested instance's changes into its own prefab, for every prefab that uses it");
            ImGui::EndDisabled();
        }
        if (m_Ctx.InPrefabMode()) return; // the stage *is* the prefab: no instance header
        Entity root = Prefabs::FindInstanceRoot(scene, e);
        if (root.IsNull()) return;
        auto& link = scene.GetRegistry().Get<PrefabLinkComponent>(root);
        AssetHandle prefab = link.Prefab;
        bool missing = AssetManager::GetMetadata(prefab) == nullptr;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, UI::AccentBackground(0.3f));
        ImGui::BeginChild("PrefabHeader", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.0f + 6.0f), ImGuiChildFlags_None);
        ImGui::TextColored(UI::AccentText(), "Prefab");
        ImGui::SameLine();
        if (missing) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "(missing asset)");
        else ImGui::TextUnformatted(AssetManager::GetDisplayName(prefab).c_str());
        if (root != e)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("- part of '%s'", scene.GetRegistry().Get<TagComponent>(root).Name.c_str());
        }
        size_t overrides = 0;
        std::vector<Entity> stack{ root };
        while (!stack.empty())
        {
            Entity cur = stack.back();
            stack.pop_back();
            if (auto* l = scene.GetRegistry().TryGet<PrefabLinkComponent>(cur); l && l->Prefab == prefab) overrides += l->Overrides.size();
            else ++overrides; // object added to the instance
            for (Entity c : scene.GetChildren(cur)) stack.push_back(c);
        }
        ImGui::BeginDisabled(missing);
        if (ImGui::SmallButton("Select")) m_Ctx.SelectAsset(prefab);
        ImGui::SameLine();
        if (ImGui::SmallButton("Open")) m_Ctx.RequestOpenPrefab = prefab;
        ImGui::SameLine();
        ImGui::BeginDisabled(m_Ctx.IsPlaying());
        if (ImGui::SmallButton("Revert"))
        {
            m_Ctx.RecordUndo();
            Prefabs::Revert(scene, root);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply"))
        {
            m_Ctx.RecordUndo();
            Prefabs::Apply(scene, root);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Unpack"))
        {
            m_Ctx.RecordUndo();
            Prefabs::Unpack(scene, root);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(overrides ? "%zu override(s)" : "no overrides", overrides);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void InspectorPanel::DrawScript(Scene& scene, Entity e)
    {
        auto& reg = scene.GetRegistry();
        EntityRefResolver resolver = MakeResolver(scene);
        int removeIndex = -1, moveFrom = -1, moveTo = -1;
        auto structural = [&] {
            m_Ctx.RecordUndo();
            Prefabs::RecordFieldOverride(scene, e, "Script", "Scripts");
        };

        auto& sc = reg.Get<ScriptComponent>(e);
        for (int i = 0; i < (int)sc.Scripts.size(); ++i)
        {
            ImGui::PushID(i);
            ScriptInstance& script = reg.Get<ScriptComponent>(e).Scripts[i];
            const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);

            // Header row: enable toggle, class name, reorder/remove buttons.
            bool enabled = script.Enabled;
            if (ImGui::Checkbox("##enabled", &enabled))
            {
                structural();
                reg.Get<ScriptComponent>(e).Scripts[i].Enabled = enabled;
            }
            ImGui::SameLine();
            std::string label = script.ClassName.empty() ? std::string("(no class)") : script.ClassName;
            if (info) label += info->Module == "csharp" ? "   C#" : "   C++";
            if (!info && !script.ClassName.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
            bool open = ImGui::TreeNodeEx("##script", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                          "%s%s", label.c_str(), !info && !script.ClassName.empty() ? "  (missing)" : "");
            if (!info && !script.ClassName.empty()) ImGui::PopStyleColor();
            float w = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - w * 3 - 8);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) { moveFrom = i; moveTo = i - 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == (int)sc.Scripts.size());
            if (ImGui::SmallButton("v")) { moveFrom = i; moveTo = i + 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeIndex = i;

            if (open)
            {
                if (UI::BeginProperties("ScriptProps"))
                {
                    UI::PropertyLabel("Class");
                    if (ImGui::BeginCombo("##class", script.ClassName.empty() ? "(none)" : script.ClassName.c_str()))
                    {
                        for (auto& s : ScriptRegistry::All())
                            if (ImGui::Selectable(s.Name.c_str(), s.Name == script.ClassName) && s.Name != script.ClassName)
                            {
                                structural();
                                auto& target = reg.Get<ScriptComponent>(e).Scripts[i];
                                target.ClassName = s.Name;
                                target.Fields = nlohmann::json::object();
                                AddRequiredComponents(scene, e, s.Name);
                            }
                        ImGui::EndCombo();
                    }
                    if (info)
                    {
                        // Edit through a temporary instance so defaults come from the class; while
                        // playing, edit the live instance (changes are discarded on Stop).
                        std::shared_ptr<ScriptableEntity> temp;
                        ScriptableEntity* target = script.Instance.get();
                        if (!target)
                        {
                            temp = info->Create();
                            target = temp.get();
                            for (auto& f : info->Fields)
                                if (script.Fields.contains(f.Name)) FieldFromJson(f, info->ToObject(target), script.Fields[f.Name], &resolver);
                        }
                        void* obj = info->ToObject(target);
                        for (auto& f : info->Fields)
                        {
                            auto& fields = reg.Get<ScriptComponent>(e).Scripts[i].Fields;
                            nlohmann::json before = fields;
                            bool overridden = Prefabs::IsOverridden(scene, e, "Script", "Scripts");
                            if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, UI::AccentText());
                            auto r = UI::Field(f, obj, &scene);
                            if (overridden) ImGui::PopStyleColor();
                            nlohmann::json after = fields;
                            if (r.Changed) after[f.Name] = FieldToJson(f, obj, &resolver);
                            fields = after;
                            ApplyEdit(m_Ctx, r, fields, before);
                            if (r.Changed) Prefabs::RecordFieldOverride(scene, e, "Script", "Scripts");
                        }
                        if (info->Fields.empty())
                        {
                            UI::PropertyLabel("");
                            ImGui::TextDisabled("No public fields");
                        }
                    }
                    UI::EndProperties();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        // Add Script
        if (ImGui::Button("+ Add Script", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("AddScript");
        if (ImGui::BeginPopup("AddScript"))
        {
            for (auto& s : ScriptRegistry::All())
                if (ImGui::Selectable(ScriptLabel(s).c_str()))
                {
                    structural();
                    reg.Get<ScriptComponent>(e).Add(s.Name);
                    AddRequiredComponents(scene, e, s.Name);
                }
            if (ScriptRegistry::All().empty()) ImGui::TextDisabled("No scripts registered");
            ImGui::EndPopup();
        }

        auto& scripts = reg.Get<ScriptComponent>(e).Scripts;
        if (removeIndex >= 0)
        {
            structural();
            scripts.erase(scripts.begin() + removeIndex);
        }
        else if (moveFrom >= 0)
        {
            structural();
            std::swap(scripts[moveFrom], scripts[moveTo]);
        }
    }

    void InspectorPanel::DrawAddComponent(Scene& scene, Entity e)
    {
        float w = 220.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f + ImGui::GetCursorPosX());
        if (ImGui::Button("Add Component", ImVec2(w, 0)))
        {
            m_ComponentFilter.clear();
            ImGui::OpenPopup("AddComponent");
        }
        if (ImGui::BeginPopup("AddComponent"))
        {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(w);
            ImGui::InputTextWithHint("##search", "Search", &m_ComponentFilter);
            std::string filter = m_ComponentFilter;
            for (auto& c : filter) c = (char)std::tolower(c);

            // Components by category; scripts in the Scripts section, or with [AddComponentMenu("Category/Name")]
            // in that category among the components.
            struct Item
            {
                const ComponentInfo* Component = nullptr;
                const ScriptInfo* Script = nullptr;
            };
            std::map<std::string, std::vector<Item>> byCategory;
            std::vector<const ScriptInfo*> plainScripts;
            auto matches = [&](std::string name) {
                for (auto& c : name) c = (char)std::tolower(c);
                return filter.empty() || name.find(filter) != std::string::npos;
            };
            for (auto& info : ComponentRegistry::All())
            {
                if (info.Hidden || !info.Removable || info.Has(scene, e) || !matches(info.Name)) continue;
                byCategory[info.Category].push_back({ &info, nullptr });
            }
            for (auto& script : ScriptRegistry::All())
            {
                if (!matches(script.Name) && !matches(script.Menu)) continue;
                size_t slash = script.Menu.find_last_of('/');
                if (slash == std::string::npos || slash == 0) plainScripts.push_back(&script);
                else byCategory[script.Menu.substr(0, slash)].push_back({ nullptr, &script });
            }
            auto addScript = [&](const ScriptInfo& script) {
                m_Ctx.RecordUndo();
                if (!scene.GetRegistry().Has<ScriptComponent>(e))
                {
                    scene.GetRegistry().Emplace<ScriptComponent>(e);
                    Prefabs::RecordComponentAdded(scene, e, "Script");
                }
                scene.GetRegistry().Get<ScriptComponent>(e).Add(script.Name);
                Prefabs::RecordFieldOverride(scene, e, "Script", "Scripts");
                AddRequiredComponents(scene, e, script.Name);
                ImGui::CloseCurrentPopup();
            };
            if (!plainScripts.empty()) ImGui::SeparatorText("Scripts");
            for (const ScriptInfo* script : plainScripts)
                if (ImGui::Selectable(ScriptLabel(*script).c_str())) addScript(*script);
            for (auto& [category, items] : byCategory)
            {
                ImGui::SeparatorText(category.c_str());
                for (auto& item : items)
                {
                    ImGui::PushID(item.Script ? (const void*)item.Script : (const void*)item.Component);
                    if (item.Script)
                    {
                        if (ImGui::Selectable(ScriptLabel(*item.Script).c_str())) addScript(*item.Script);
                    }
                    else if (ImGui::Selectable(item.Component->Name.c_str()))
                    {
                        m_Ctx.RecordUndo();
                        item.Component->Add(scene, e);
                        Prefabs::RecordComponentAdded(scene, e, item.Component->Name);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }
    }

    void InspectorPanel::DrawAsset(AssetHandle handle)
    {
        const AssetMetadata* meta = AssetManager::GetMetadata(handle);
        if (!meta)
        {
            ImGui::TextDisabled("Asset no longer exists.");
            return;
        }
        ImGui::Text("%s", meta->Path.filename().string().c_str());
        ImGui::TextDisabled("%s  |  %s", AssetTypeToString(meta->Type), meta->Path.generic_string().c_str());
        ImGui::TextDisabled("GUID %llu", (unsigned long long)(uint64_t)handle);
        ImGui::Separator();

        if (meta->Type == AssetType::Audio)
        {
            if (auto info = AudioEngine::GetClipInfo(AssetManager::GetAbsolutePath(handle)))
            {
                ImGui::Text("%s, %s, %u Hz", info->Format.c_str(), info->Channels == 1 ? "mono" : info->Channels == 2 ? "stereo" : "multichannel",
                            info->SampleRate);
                ImGui::Text("Length %d:%05.2f", (int)(info->Seconds / 60.0f), std::fmod(info->Seconds, 60.0f));
            }
            else
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "This file could not be decoded.");
        }
        if (meta->Type == AssetType::Texture)
        {
            nlohmann::json settings = meta->ImportSettings;
            bool changed = false;
            ImGui::SeparatorText("Import Settings");
            int filter = settings.value("filter", 0);
            const char* filters[] = { "Bilinear", "Point (no filter)" };
            if (ImGui::Combo("Filter Mode", &filter, filters, 2)) { settings["filter"] = filter; changed = true; }
            bool srgb = settings.value("srgb", true);
            if (ImGui::Checkbox("sRGB (Color Texture)", &srgb)) { settings["srgb"] = srgb; changed = true; }
            bool mips = settings.value("mips", true);
            if (ImGui::Checkbox("Generate Mip Maps", &mips)) { settings["mips"] = mips; changed = true; }
            if (changed) AssetManager::SetImportSettings(handle, settings);

            if (auto tex = AssetManager::GetTexture(handle))
            {
                ImGui::SeparatorText("Preview");
                float avail = ImGui::GetContentRegionAvail().x;
                float aspect = (float)tex->GetHeight() / std::max(1u, tex->GetWidth());
                ImGui::Image(ImTextureRef((ImTextureID)tex->GetImGuiId()), ImVec2(avail, avail * aspect));
                ImGui::TextDisabled("%u x %u", tex->GetWidth(), tex->GetHeight());
            }
        }
        else if (meta->Type == AssetType::Mesh)
        {
            // Import settings: re-import (and refresh every user of the model) when they change.
            ImGui::SeparatorText("Import Settings");
            nlohmann::json settings = meta->ImportSettings;
            MeshImportSettings current = AssetManager::GetMeshImportSettings(handle);
            bool changed = false;
            float scale = current.Scale;
            ImGui::DragFloat("Scale", &scale, 0.01f, 0.0001f, 10000.0f, "%.4g");
            if (ImGui::IsItemDeactivatedAfterEdit() && scale != current.Scale) { settings["scale"] = scale; changed = true; }
            ImGui::SetItemTooltip("Uniform scale applied on import (e.g. 0.01 for a model made in centimeters)");
            bool animations = current.ImportAnimations, materials = current.ImportMaterials;
            if (ImGui::Checkbox("Import Animations", &animations)) { settings["animations"] = animations; changed = true; }
            if (ImGui::Checkbox("Import Materials", &materials)) { settings["importMaterials"] = materials; changed = true; }
            ImGui::SetItemTooltip("Off: every part uses the Mesh Renderer's own material");
            if (animations)
            {
                // Which clips to import: the imported ones, plus the excluded ones (known from the settings).
                std::vector<std::pair<std::string, bool>> clips;
                if (auto mesh = AssetManager::GetMesh(handle))
                    for (const auto& c : mesh->GetClips()) clips.push_back({ c.Name, true });
                for (const auto& name : current.ExcludedClips)
                    if (std::none_of(clips.begin(), clips.end(), [&](const auto& c) { return c.first == name; })) clips.push_back({ name, false });
                if (!clips.empty() && ImGui::TreeNodeEx("Clips", ImGuiTreeNodeFlags_DefaultOpen, "Clips (%zu)", clips.size()))
                {
                    for (auto& [name, included] : clips)
                    {
                        bool on = included;
                        ImGui::PushID(name.c_str());
                        if (ImGui::Checkbox(name.empty() ? "(unnamed)" : name.c_str(), &on))
                        {
                            nlohmann::json excluded = nlohmann::json::array();
                            for (const auto& other : current.ExcludedClips)
                                if (other != name) excluded.push_back(other);
                            if (!on) excluded.push_back(name);
                            if (excluded.empty()) settings.erase("excludedClips");
                            else settings["excludedClips"] = excluded;
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            }
            if (changed) AssetManager::SetImportSettings(handle, settings);
            if (auto mesh = AssetManager::GetMesh(handle))
            {
                ImGui::Text("Vertices: %u", mesh->GetVertexCount());
                ImGui::Text("Triangles: %u", mesh->GetIndexCount() / 3);
                auto ext = mesh->GetBounds().Extents() * 2.0f;
                ImGui::Text("Bounds: %.2f x %.2f x %.2f", ext.x, ext.y, ext.z);
                ImGui::Text("Sub-meshes: %zu", mesh->GetSubMeshes().size());
                const auto& slots = AssetManager::GetMeshMaterials(handle);
                if (!slots.empty())
                {
                    ImGui::SeparatorText("Materials");
                    auto names = AssetManager::GetMeshMaterialNames(handle);
                    bool extracted = AssetManager::AreMeshMaterialsExtracted(handle);
                    for (size_t i = 0; i < slots.size(); ++i)
                    {
                        ImGui::PushID((int)i);
                        ImGui::BulletText("%s", i < names.size() ? names[i].c_str() : "?");
                        ImGui::SameLine();
                        if (extracted && !AssetManager::IsSubAsset(slots[i]))
                        {
                            if (ImGui::SmallButton("Select")) m_Ctx.SelectAsset(slots[i]);
                        }
                        else
                            ImGui::TextDisabled("(embedded)");
                        ImGui::PopID();
                    }
                    if (!extracted)
                    {
                        ImGui::TextWrapped("Embedded materials are read-only. Extract them to edit them as .mat assets "
                                           "(textures embedded in the file are written out too).");
                        if (ImGui::Button("Extract Materials")) AssetManager::ExtractMeshMaterials(handle);
                    }
                    else
                        ImGui::TextDisabled("Using extracted materials. A Mesh Renderer's Material overrides all slots.");
                }
                if (const Skeleton* sk = mesh->GetSkeleton())
                {
                    ImGui::SeparatorText("Skeleton");
                    ImGui::Text("%zu joints", sk->Joints.size());
                    if (ImGui::TreeNode("Joints"))
                    {
                        for (auto& j : sk->Joints)
                            ImGui::BulletText("%s%s", j.Name.c_str(), j.Parent < 0 ? "  (root)" : "");
                        ImGui::TreePop();
                    }
                    ImGui::SeparatorText("Animation Clips");
                    for (auto& c : mesh->GetClips()) ImGui::BulletText("%s  (%.2f s, %zu channels)", c.Name.c_str(), c.Duration, c.Channels.size());
                    if (mesh->GetClips().empty()) ImGui::TextDisabled("No clips");
                }
            }
        }
        else if (meta->Type == AssetType::Material)
        {
            auto material = AssetManager::GetMaterial(handle);
            if (!material) return;
            bool save = false;
            if (UI::BeginProperties("MaterialProps"))
            {
                for (auto& f : Material::Fields())
                {
                    AssetHandle normalBefore = material->NormalMap;
                    auto r = UI::Field(f, material.get(), nullptr);
                    if (r.Changed) save = true;
                    // Normal maps hold vectors, not colors: make sure they are imported as linear.
                    if (material->NormalMap != normalBefore && material->NormalMap.IsValid())
                    {
                        if (auto* tm = AssetManager::GetMetadata(material->NormalMap); tm && tm->ImportSettings.value("srgb", true))
                        {
                            auto settings = tm->ImportSettings;
                            settings["srgb"] = false;
                            AssetManager::SetImportSettings(material->NormalMap, settings);
                            Log::Info("Normal map '{}' switched to linear (non-sRGB) import", tm->Path.filename().string());
                        }
                    }
                }
                UI::EndProperties();
            }
            if (save) AssetManager::SaveMaterial(handle);
            ImGui::TextDisabled("Changes are saved to the .mat file immediately.");
        }
        else if (meta->Type == AssetType::AnimatorController)
        {
            if (auto c = AssetManager::GetAnimatorController(handle))
            {
                ImGui::Text("%zu states, %zu transitions, %zu parameters", c->States.size(), c->Transitions.size(), c->Parameters.size());
                if (!c->States.empty() && c->DefaultState < (int)c->States.size())
                    ImGui::TextDisabled("Default state: %s", c->States[c->DefaultState].Name.c_str());
                ImGui::TextWrapped("Edit states, transitions and parameters in the Animator window (Window > Animator). "
                                   "Assign the controller to an Animator component to use it.");
            }
        }
        else if (meta->Type == AssetType::Scene)
        {
            if (ImGui::Button("Open Scene")) m_Ctx.RequestOpenScene = AssetManager::GetAbsolutePath(handle);
        }
        else if (meta->Type == AssetType::Prefab)
        {
            if (ImGui::Button("Open Prefab")) m_Ctx.RequestOpenPrefab = handle;
            if (AssetHandle base = Prefabs::GetVariantBase(handle); base.IsValid())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("Variant of");
                ImGui::SameLine();
                if (ImGui::SmallButton(AssetManager::GetDisplayName(base).c_str())) m_Ctx.SelectedAsset = base;
                ImGui::TextWrapped("A variant keeps only what it changes; everything else follows its base prefab.");
            }
            ImGui::TextWrapped("Opens the prefab on its own (Prefab Mode); double-clicking it in the Project panel does the same. "
                               "Drag it into the Scene view or Hierarchy to instantiate it, or reference it from a script field.");
        }
    }

    void SceneSettingsPanel::OnImGui(bool* open)
    {
        if (!ImGui::Begin("Scene Settings", open))
        {
            ImGui::End();
            return;
        }
        Scene& scene = m_Ctx.ActiveScene();
        auto& s = scene.GetSettings();
        SceneSettings before = s;
        UI::EditResult r;
        auto track = [&](bool changed) { r.Changed |= changed; r.Activated |= ImGui::IsItemActivated(); };
        ImGui::SeparatorText("Environment");
        track(ImGui::ColorEdit3("Ambient Color", &s.AmbientColor.x, ImGuiColorEditFlags_Float));
        track(ImGui::DragFloat("Ambient Intensity", &s.AmbientIntensity, 0.01f, 0.0f, 10.0f));
        ImGui::SeparatorText("Sky");
        track(ImGui::Combo("Sky Mode", &s.SkyMode, "Procedural\0Texture (HDR)\0"));
        if (s.SkyMode == 1)
        {
            if (UI::BeginProperties("SkyTexture"))
            {
                UI::EditResult tex = UI::AssetField("Sky Texture", s.SkyTexture, AssetType::Texture);
                r |= tex;
                UI::EndProperties();
            }
            track(ImGui::DragFloat("Sky Intensity", &s.SkyIntensity, 0.01f, 0.0f, 20.0f));
            track(ImGui::DragFloat("Sky Rotation", &s.SkyRotation, 0.5f, -360.0f, 360.0f, "%.1f deg"));
            track(ImGui::DragFloat("Reflection Intensity", &s.ReflectionIntensity, 0.01f, 0.0f, 10.0f));
            ImGui::TextDisabled("Ambient Intensity scales the sky's diffuse light.");
            auto tex = AssetManager::GetTexture(s.SkyTexture);
            if (!tex) ImGui::TextDisabled("Assign an equirectangular .hdr image.");
            else if (!tex->IsHDR()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Not HDR: reflections only, ambient uses Ambient Color.");
        }
        else
        {
            track(ImGui::ColorEdit3("Sky Top", &s.SkyTop.x, ImGuiColorEditFlags_Float));
            track(ImGui::ColorEdit3("Sky Horizon", &s.SkyHorizon.x, ImGuiColorEditFlags_Float));
            track(ImGui::ColorEdit3("Sky Ground", &s.SkyGround.x, ImGuiColorEditFlags_Float));
        }
        ImGui::SeparatorText("Post Processing");
        track(ImGui::DragFloat("Exposure", &s.Exposure, 0.01f, 0.0f, 20.0f));
        track(ImGui::Checkbox("ACES Tonemapping", &s.Tonemapping));
        track(ImGui::Checkbox("Bloom", &s.Bloom));
        if (s.Bloom)
        {
            ImGui::Indent();
            track(ImGui::DragFloat("Intensity", &s.BloomIntensity, 0.01f, 0.0f, 20.0f));
            track(ImGui::DragFloat("Threshold", &s.BloomThreshold, 0.01f, 0.0f, 50.0f));
            track(ImGui::SliderFloat("Soft Knee", &s.BloomKnee, 0.0f, 1.0f));
            track(ImGui::SliderFloat("Radius", &s.BloomRadius, 0.5f, 3.0f));
            ImGui::Unindent();
        }
        track(ImGui::Checkbox("Ambient Occlusion (SSAO)", &s.SSAO));
        if (s.SSAO)
        {
            ImGui::Indent();
            track(ImGui::DragFloat("Radius##ssao", &s.SSAORadius, 0.01f, 0.05f, 5.0f));
            track(ImGui::DragFloat("Intensity##ssao", &s.SSAOIntensity, 0.01f, 0.0f, 4.0f));
            track(ImGui::DragFloat("Bias##ssao", &s.SSAOBias, 0.001f, 0.0f, 0.5f, "%.3f"));
            track(ImGui::Checkbox("Show Occlusion##ssao", &s.SSAODebug));
            ImGui::Unindent();
        }
        track(ImGui::Checkbox("Screen-Space Reflections", &s.SSR));
        ImGui::SetItemTooltip("Smooth surfaces reflect what is on screen (perspective cameras). Off screen, behind\n"
                              "objects and on rough surfaces, reflection probes and the sky take over.");
        if (s.SSR)
        {
            ImGui::Indent();
            track(ImGui::DragFloat("Intensity##ssr", &s.SSRIntensity, 0.01f, 0.0f, 2.0f));
            track(ImGui::DragFloat("Max Distance##ssr", &s.SSRMaxDistance, 0.1f, 0.5f, 500.0f));
            track(ImGui::DragFloat("Thickness##ssr", &s.SSRThickness, 0.01f, 0.01f, 10.0f));
            track(ImGui::SliderFloat("Max Roughness##ssr", &s.SSRMaxRoughness, 0.05f, 1.0f));
            track(ImGui::SliderInt("Steps##ssr", &s.SSRSteps, 8, 256));
            ImGui::Unindent();
        }
        track(ImGui::Checkbox("Depth of Field", &s.DepthOfField));
        if (s.DepthOfField)
        {
            ImGui::Indent();
            track(ImGui::DragFloat("Focus Distance", &s.FocusDistance, 0.05f, 0.05f, 10000.0f));
            track(ImGui::DragFloat("Focus Range", &s.FocusRange, 0.05f, 0.05f, 10000.0f));
            track(ImGui::SliderFloat("Bokeh Size", &s.BokehSize, 0.0f, 0.04f, "%.3f"));
            ImGui::Unindent();
        }
        track(ImGui::SliderFloat("Vignette", &s.Vignette, 0.0f, 1.0f));
        track(ImGui::Checkbox("FXAA (anti-aliasing)", &s.FXAA));
        {
            int msaa = s.MSAA >= 8 ? 3 : s.MSAA >= 4 ? 2 : s.MSAA >= 2 ? 1 : 0;
            if (ImGui::Combo("MSAA", &msaa, "Off\0" "2x\0" "4x\0" "8x\0"))
            {
                s.MSAA = 1 << msaa;
                r.Changed = true;
                r.Activated = true;
            }
            ImGui::SetItemTooltip("Multisample anti-aliasing: smooth geometry edges by shading each pixel once but testing\n"
                                  "coverage at several points. Costs memory and fill rate; combines with FXAA.");
        }
        track(ImGui::Checkbox("TAA (temporal anti-aliasing)", &s.TAA));
        ImGui::SetItemTooltip("Renders each frame with a sub-pixel offset and blends it with the previous frames: smooths\n"
                              "edges, thin lines and shimmering detail (game views; the Scene view stays unjittered).");
        if (s.TAA)
        {
            ImGui::Indent();
            track(ImGui::SliderFloat("New Frame Weight##taa", &s.TAAFeedback, 0.02f, 0.5f));
            ImGui::Unindent();
        }
        track(ImGui::Checkbox("Volumetric Fog", &s.Fog));
        if (s.Fog)
        {
            ImGui::Indent();
            track(ImGui::ColorEdit3("Fog Color", &s.FogColor.x));
            track(ImGui::DragFloat("Intensity##fog", &s.FogIntensity, 0.01f, 0.0f, 20.0f));
            track(ImGui::DragFloat("Density##fog", &s.FogDensity, 0.001f, 0.0f, 2.0f, "%.4f"));
            track(ImGui::DragFloat("Base Height##fog", &s.FogHeight, 0.05f));
            track(ImGui::DragFloat("Height Falloff##fog", &s.FogHeightFalloff, 0.005f, 0.0f, 10.0f, "%.3f"));
            track(ImGui::DragFloat("Max Distance##fog", &s.FogMaxDistance, 0.5f, 1.0f, 10000.0f));
            track(ImGui::DragFloat("Sun Scattering##fog", &s.FogSunScattering, 0.01f, 0.0f, 20.0f));
            track(ImGui::SliderFloat("Anisotropy##fog", &s.FogAnisotropy, -0.9f, 0.9f));
            track(ImGui::SliderInt("Steps##fog", &s.FogSteps, 4, 128));
            ImGui::Unindent();
        }
        ImGui::SeparatorText("UI");
        track(ImGui::Checkbox("Keyboard Navigation", &s.UIKeyboardNavigation));
        ImGui::SetItemTooltip("Arrows move the UI selection and Enter/Space press it. Turn off when gameplay uses those keys; "
                              "the gamepad (D-pad, left stick, A/B) always navigates.");
        ImGui::SeparatorText("Physics");
        track(ImGui::DragFloat2("Gravity 2D", &s.Gravity2D.x, 0.05f));
        track(ImGui::DragFloat3("Gravity 3D", &s.Gravity.x, 0.05f));
        ApplyEdit(m_Ctx, r, s, before);
        std::string name = scene.GetName();
        ImGui::SeparatorText("Scene");
        if (ImGui::InputText("Name", &name))
        {
            scene.SetName(name);
            m_Ctx.MarkDirty();
        }
        ImGui::End();
    }
}
