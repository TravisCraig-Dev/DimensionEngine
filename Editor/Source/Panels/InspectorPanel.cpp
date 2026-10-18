#include "Panels.h"
#include "../EditorUI.h"
#include <Dimension/Scene/ComponentRegistry.h>
#include <Dimension/Scripting/ScriptRegistry.h>
#include <Dimension/Scripting/ScriptableEntity.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Animation/AnimatorController.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <map>

namespace Editor
{
    namespace
    {
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

        bool ComponentHeader(const char* name, bool removable, bool& remove, bool& reset)
        {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 5));
            bool open = ImGui::TreeNodeEx(name, flags, "%s", name);
            ImGui::PopStyleVar();
            float w = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
            ImGui::PushID(name);
            if (ImGui::Button("...", ImVec2(w, w))) ImGui::OpenPopup("ComponentSettings");
            if (ImGui::BeginPopup("ComponentSettings"))
            {
                if (ImGui::MenuItem("Reset")) reset = true;
                if (ImGui::MenuItem("Remove Component", nullptr, false, removable)) remove = true;
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
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%zu objects selected", count);
                ImGui::TextDisabled("Showing the active object; edits apply to all selected objects with the same component.");
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
            UI::EditResult r;
            r.Changed = ImGui::Checkbox("##active", &tag.Active);
            r.Activated = ImGui::IsItemActivated();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            r.Changed |= ImGui::InputText("##name", &tag.Name);
            r.Activated |= ImGui::IsItemActivated();
            ImGui::TextDisabled("Tag");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("##tag", tag.Tag.c_str()))
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
            ImGui::TextDisabled("ID %llu", (unsigned long long)(uint64_t)scene.GetGuid(e));
            ApplyEdit(m_Ctx, r, tag, before);
            if (r.Changed) scene.UpdateTransforms();
        }
        ImGui::Separator();
        DrawPrefabHeader(scene, e);
        EntityRefResolver resolver = MakeResolver(scene);

        const ComponentInfo* removeInfo = nullptr;
        for (auto& info : ComponentRegistry::All())
        {
            if (info.Hidden || !info.Has(scene, e)) continue;
            bool remove = false, reset = false;
            bool open = ComponentHeader(info.Name.c_str(), info.Removable, remove, reset);
            if (open)
            {
                if (info.TypeId == GetComponentTypeId<ScriptComponent>())
                    DrawScript(scene, e);
                else if (UI::BeginProperties(info.Name.c_str()))
                {
                    void* data = info.Get(scene, e);
                    for (auto& f : info.Fields)
                    {
                        nlohmann::json before = FieldToJson(f, data, &resolver);
                        UI::EditResult r;
                        bool handled = false;
                        if (info.TypeId == GetComponentTypeId<AnimatorComponent>() && f.Name == "Clip")
                        {
                            // Offer the clips of the entity's skinned mesh.
                            auto* mr = reg.TryGet<MeshRendererComponent>(e);
                            auto mesh = mr ? AssetManager::GetMesh(mr->Mesh) : nullptr;
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
                        bool overridden = Prefabs::IsOverridden(scene, e, info.Name, f.Name);
                        if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f));
                        if (!handled) r = UI::Field(f, data, &scene);
                        if (overridden) ImGui::PopStyleColor();
                        if (r.Changed)
                            for (Entity target : m_Ctx.Selection)
                                if (scene.IsValid(target) && info.Has(scene, target)) Prefabs::RecordFieldOverride(scene, target, info.Name, f.Name);
                        if (r.Changed && !m_Ctx.IsSelected(e)) Prefabs::RecordFieldOverride(scene, e, info.Name, f.Name);
                        ApplyFieldEdit(m_Ctx, r, f, data, before, resolver);
                        if (r.Changed)
                        {
                            // Multi-edit: copy the new value to the same field on every other selected object.
                            nlohmann::json value = FieldToJson(f, data, &resolver);
                            for (Entity other : m_Ctx.Selection)
                                if (other != e && scene.IsValid(other))
                                    if (void* od = info.Get(scene, other)) FieldFromJson(f, od, value, &resolver);
                            data = info.Get(scene, e);
                            scene.UpdateTransforms();
                        }
                    }
                    UI::EndProperties();
                }
                ImGui::TreePop();
            }
            if (reset)
            {
                m_Ctx.RecordUndo();
                info.Remove(scene, e);
                info.Add(scene, e);
                for (auto& f : info.Fields) Prefabs::RecordFieldOverride(scene, e, info.Name, f.Name);
            }
            if (remove) removeInfo = &info;
            ImGui::Spacing();
        }
        if (removeInfo)
        {
            m_Ctx.RecordUndo();
            removeInfo->Remove(scene, e);
            Prefabs::RecordComponentRemoved(scene, e, removeInfo->Name);
        }

        ImGui::Separator();
        DrawAddComponent(scene, e);
        ImGui::PopID();
    }

    void InspectorPanel::DrawPrefabHeader(Scene& scene, Entity e)
    {
        Entity root = Prefabs::FindInstanceRoot(scene, e);
        if (root.IsNull()) return;
        auto& link = scene.GetRegistry().Get<PrefabLinkComponent>(root);
        AssetHandle prefab = link.Prefab;
        bool missing = AssetManager::GetMetadata(prefab) == nullptr;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.26f, 0.38f, 1.0f));
        ImGui::BeginChild("PrefabHeader", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.0f + 6.0f), ImGuiChildFlags_None);
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "Prefab");
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
                            if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f));
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
                if (ImGui::Selectable((s.Name + (s.Module == "csharp" ? "   (C#)" : "   (C++)")).c_str()))
                {
                    structural();
                    reg.Get<ScriptComponent>(e).Add(s.Name);
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

            std::map<std::string, std::vector<const ComponentInfo*>> byCategory;
            for (auto& info : ComponentRegistry::All())
            {
                if (info.Hidden || !info.Removable || info.Has(scene, e)) continue;
                std::string name = info.Name;
                for (auto& c : name) c = (char)std::tolower(c);
                if (!filter.empty() && name.find(filter) == std::string::npos) continue;
                byCategory[info.Category].push_back(&info);
            }
            bool anyScript = false;
            for (auto& script : ScriptRegistry::All())
            {
                std::string name = script.Name;
                for (auto& c : name) c = (char)std::tolower(c);
                if (!filter.empty() && name.find(filter) == std::string::npos) continue;
                if (!anyScript) ImGui::SeparatorText("Scripts");
                anyScript = true;
                if (ImGui::Selectable((script.Name + (script.Module == "csharp" ? "   (C#)" : "   (C++)")).c_str()))
                {
                    m_Ctx.RecordUndo();
                    if (!scene.GetRegistry().Has<ScriptComponent>(e))
                    {
                        scene.GetRegistry().Emplace<ScriptComponent>(e);
                        Prefabs::RecordComponentAdded(scene, e, "Script");
                    }
                    scene.GetRegistry().Get<ScriptComponent>(e).Add(script.Name);
                    Prefabs::RecordFieldOverride(scene, e, "Script", "Scripts");
                    ImGui::CloseCurrentPopup();
                }
            }
            for (auto& [category, infos] : byCategory)
            {
                ImGui::SeparatorText(category.c_str());
                for (auto* info : infos)
                    if (ImGui::Selectable(info->Name.c_str()))
                    {
                        m_Ctx.RecordUndo();
                        info->Add(scene, e);
                        Prefabs::RecordComponentAdded(scene, e, info->Name);
                        ImGui::CloseCurrentPopup();
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
            if (auto mesh = AssetManager::GetMesh(handle))
            {
                ImGui::Text("Vertices: %u", mesh->GetVertexCount());
                ImGui::Text("Triangles: %u", mesh->GetIndexCount() / 3);
                auto ext = mesh->GetBounds().Extents() * 2.0f;
                ImGui::Text("Bounds: %.2f x %.2f x %.2f", ext.x, ext.y, ext.z);
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
            ImGui::TextWrapped("Drag this prefab into the Scene view or Hierarchy to instantiate it, or reference it from a script field.");
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
        track(ImGui::ColorEdit3("Sky Top", &s.SkyTop.x, ImGuiColorEditFlags_Float));
        track(ImGui::ColorEdit3("Sky Horizon", &s.SkyHorizon.x, ImGuiColorEditFlags_Float));
        track(ImGui::ColorEdit3("Sky Ground", &s.SkyGround.x, ImGuiColorEditFlags_Float));
        ImGui::SeparatorText("Post Processing");
        track(ImGui::DragFloat("Exposure", &s.Exposure, 0.01f, 0.0f, 20.0f));
        track(ImGui::Checkbox("ACES Tonemapping", &s.Tonemapping));
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
