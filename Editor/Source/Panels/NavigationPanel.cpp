#include "Panels.h"
#include <Dimension/Navigation/NavMesh.h>
#include <Dimension/Project/Project.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace Editor
{
    namespace
    {
        // As drawn in the Scene view (SceneRenderer).
        const ImVec4 kAreaColors[8] = { { 0.35f, 0.7f, 1.0f, 1.0f }, { 1.0f, 0.3f, 0.3f, 1.0f }, { 0.75f, 0.75f, 0.8f, 1.0f }, { 0.4f, 0.9f, 0.35f, 1.0f },
                                        { 0.7f, 0.45f, 0.2f, 1.0f }, { 0.2f, 0.85f, 0.95f, 1.0f }, { 0.9f, 0.5f, 0.9f, 1.0f }, { 0.95f, 0.85f, 0.3f, 1.0f } };
    }

    // Window > Navigation (like Unity's AI Navigation window): the scene's NavMesh Surfaces (bake them all or one by
    // one), its agents and the project's path budget, and the project's navigation areas (names and default costs).
    // While it is open the Scene view shows every baked navigation mesh.
    void NavigationPanel::OnImGui(bool* open)
    {
        // Layouts saved before the window existed leave it floating: start it a useful size, near the middle.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(440.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Navigation", open))
        {
            ImGui::End();
            return;
        }
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        auto& config = Project::GetConfig();

        ImGui::SeparatorText("Surfaces");
        std::vector<Entity> surfaces;
        for (auto [e, s] : reg.ViewOf<NavMeshSurfaceComponent>()) surfaces.push_back(e);
        if (surfaces.empty())
            ImGui::TextDisabled("No NavMesh Surface in the scene (Add Component > Navigation > NavMesh Surface).");
        else
        {
            if (ImGui::Button("Bake All"))
                for (Entity e : surfaces) m_Ctx.BakeNavMesh(e);
            ImGui::SetItemTooltip("Bakes every surface and saves the bakes with the scene: play starts with them");
            ImGui::SameLine();
            if (ImGui::Button("Clear All"))
                for (Entity e : surfaces) m_Ctx.ClearNavMesh(e);
            ImGui::SameLine();
            ImGui::TextDisabled("(surfaces without a saved bake bake at play)");
            if (ImGui::BeginTable("##surfaces", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Surface");
                ImGui::TableSetupColumn("Spots");
                ImGui::TableSetupColumn("Links");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (Entity e : surfaces)
                {
                    auto& s = reg.Get<NavMeshSurfaceComponent>(e);
                    ImGui::PushID((int)e.Index);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(reg.Get<TagComponent>(e).Name.c_str(), m_Ctx.Selected == e)) m_Ctx.Select(e);
                    ImGui::TableNextColumn();
                    if (s.Data) ImGui::Text("%d", s.Data->NodeCount());
                    else ImGui::TextDisabled(s.BakedData.IsValid() ? "saved" : "not baked");
                    if (s.BakedData.IsValid() && !s.Streamed && !m_Ctx.IsPlaying())
                    {
                        ImGui::SameLine();
                        bool current = Navigation::IsSavedCurrent(scene, e);
                        ImGui::TextColored(current ? ImVec4(0.45f, 0.8f, 0.5f, 1.0f) : ImVec4(0.95f, 0.75f, 0.3f, 1.0f), current ? "(saved)" : "(out of date)");
                    }
                    ImGui::TableNextColumn();
                    if (s.Data) ImGui::Text("%zu", s.Data->AutoLinks.size());
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Bake")) m_Ctx.BakeNavMesh(e);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        ImGui::SeparatorText("Agents");
        int agents = 0, walking = 0;
        for (auto [e, a] : reg.ViewOf<NavMeshAgentComponent>())
        {
            ++agents;
            walking += a.HasDestination && a.RemainingDistance > a.StoppingDistance + 0.05f;
        }
        if (m_Ctx.IsPlaying())
            ImGui::Text("%d agent%s, %d walking", agents, agents == 1 ? "" : "s", walking);
        else
            ImGui::Text("%d agent%s", agents, agents == 1 ? "" : "s");
        int budget = Navigation::GetPathBudget();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::DragInt("Path Budget", &budget, 0.2f, 1, 1000)) Navigation::SetPathBudget(budget);
        if (ImGui::IsItemDeactivatedAfterEdit()) Project::Save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("New paths agents may find a frame (following targets, around moved obstacles);\nthe rest wait their turn on their old path");

        ImGui::SeparatorText("Areas");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("NavMesh Modifier Volumes mark areas; Cost 0 on a volume takes the area's cost here.");
        ImGui::PopTextWrapPos();
        if (ImGui::BeginTable("##areas", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Cost");
            ImGui::TableHeadersRow();
            for (int i = 0; i < ProjectConfig::kNavAreas; ++i)
            {
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::ColorButton("##color", kAreaColors[i], ImGuiColorEditFlags_NoTooltip, ImVec2(14.0f, 14.0f));
                ImGui::SameLine();
                ImGui::Text("%d", i);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##name", &config.NavAreaNames[i])) Navigation::ApplyAreaNames();
                if (ImGui::IsItemDeactivatedAfterEdit()) Project::Save();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (i == Navigation::kNotWalkable)
                    ImGui::TextDisabled("(not walked)");
                else
                {
                    ImGui::DragFloat("##cost", &config.NavAreaCosts[i], 0.05f, 0.05f, 100.0f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) Project::Save();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Agents' Area Mask has a bit per area: 1 << number.");

        // Agent types: a surface of a type bakes with its sizes; an agent of a type walks only surfaces of it (or Any).
        ImGui::SeparatorText("Agent Types");
        bool typesChanged = false, typesEdited = false;
        int remove = -1;
        if (ImGui::BeginTable("##agentTypes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Radius");
            ImGui::TableSetupColumn("Height");
            ImGui::TableSetupColumn("Step");
            ImGui::TableSetupColumn("Slope");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)config.NavAgentTypes.size(); ++i)
            {
                auto& type = config.NavAgentTypes[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                typesChanged |= ImGui::InputText("##name", &type.Name);
                typesEdited |= ImGui::IsItemDeactivatedAfterEdit();
                float* values[4] = { &type.Radius, &type.Height, &type.StepHeight, &type.MaxSlope };
                const float limits[4] = { 10.0f, 20.0f, 5.0f, 89.0f };
                for (int k = 0; k < 4; ++k)
                {
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::PushID(k);
                    ImGui::DragFloat("##v", values[k], k == 3 ? 0.5f : 0.01f, 0.0f, limits[k], k == 3 ? "%.0f deg" : "%.2f");
                    typesEdited |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) remove = i;
                ImGui::SetItemTooltip("Remove the type (surfaces and agents of it become Any; types after it move up)");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (remove >= 0)
        {
            // Components keep their place in the list: those of the removed type become Any, later ones move up.
            config.NavAgentTypes.erase(config.NavAgentTypes.begin() + remove);
            auto renumber = [&](int32_t& type) { type = type == remove + 1 ? 0 : type > remove + 1 ? type - 1 : type; };
            for (auto [e, s] : reg.ViewOf<NavMeshSurfaceComponent>()) renumber(s.AgentType);
            for (auto [e, a] : reg.ViewOf<NavMeshAgentComponent>()) renumber(a.AgentType);
            m_Ctx.MarkDirty();
            typesChanged = typesEdited = true;
        }
        if (ImGui::Button("+ Agent Type"))
        {
            ProjectConfig::NavAgentType type;
            type.Name = "Agent Type " + std::to_string(config.NavAgentTypes.size() + 1);
            config.NavAgentTypes.push_back(type);
            typesChanged = typesEdited = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Bake again after changing a type's sizes.");
        if (typesChanged) Navigation::ApplyAreaNames();
        if (typesEdited) Project::Save();
        ImGui::End();
    }
}
