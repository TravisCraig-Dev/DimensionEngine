#include "Panels.h"
#include <imgui.h>
#include <algorithm>

namespace Editor
{
    int FrameDebuggerPanel::StopFor(int view) const
    {
        if (!m_Enabled || view != m_View) return -2;
        return m_Event;
    }

    // Window > Frame Debugger (like Unity's): while enabled, the chosen view's frame as a list of its draws, pass by
    // pass (shadows, depth prepass, opaque meshes, sky, sprites, particles, transparent meshes, decals, UI). Picking a
    // draw (or stepping with the arrows) shows the view as it stood right after it: the later draws are left out
    // (effects and post processing still run). The draw's object can be selected in the Hierarchy.
    void FrameDebuggerPanel::OnImGui(bool* open, const std::vector<FrameDebugEvent>& events)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(620.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Frame Debugger", open))
        {
            ImGui::End();
            return;
        }
        if (ImGui::Button(m_Enabled ? "Disable" : "Enable"))
        {
            m_Enabled = !m_Enabled;
            m_Event = -1;
            m_ScrollTo = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const char* views[] = { "Scene View", "Game View" };
        if (ImGui::Combo("##view", &m_View, views, IM_ARRAYSIZE(views))) m_Event = -1;
        if (!m_Enabled)
        {
            ImGui::TextDisabled("Enable to list the draws of the chosen view's frame and step through them.");
            ImGui::End();
            return;
        }
        int count = (int)events.size();
        if (count == 0)
        {
            ImGui::TextDisabled(m_View == 1 ? "No frame yet: open the Game view." : "No frame yet.");
            ImGui::End();
            return;
        }
        // Stepping: the last draw shown (all of them by default).
        int shown = m_Event < 0 ? count - 1 : std::min(m_Event, count - 1);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##previous", ImGuiDir_Left) && shown > 0)
        {
            m_Event = shown - 1;
            m_ScrollTo = true;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Right) && shown + 1 < count)
        {
            m_Event = shown + 1 == count - 1 ? -1 : shown + 1;
            m_ScrollTo = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        int slider = shown + 1;
        if (ImGui::SliderInt("##event", &slider, 1, count, "Draw %d"))
        {
            m_Event = slider >= count ? -1 : slider - 1;
            m_ScrollTo = true;
        }
        ImGui::TextDisabled("%d of %d draws", shown + 1, count);

        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        auto entityOf = [&](uint32_t id) {
            if (id == 0) return Entity::Null();
            Entity e = reg.EntityFromIndex(id - 1);
            return scene.IsValid(e) ? e : Entity::Null();
        };
        auto nameOf = [&](uint32_t id) -> std::string {
            Entity e = entityOf(id);
            if (e.IsNull()) return id ? "(gone)" : "";
            const auto* tag = reg.TryGet<TagComponent>(e);
            return tag ? tag->Name : "Entity";
        };

        // The picked draw.
        const FrameDebugEvent& at = events[shown];
        ImGui::SeparatorText("Draw");
        if (ImGui::BeginTable("##detail", 2, ImGuiTableFlags_SizingStretchProp))
        {
            auto row = [](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value.c_str());
            };
            row("Pass", at.Pass);
            row("Target", at.Target);
            row("Draws", at.What.empty() ? std::string("(unnamed mesh)") : at.What);
            row("Object", at.EntityId ? nameOf(at.EntityId) : std::string("-"));
            row("Triangles", std::to_string(at.Triangles));
            row("Instances", std::to_string(at.Instances));
            ImGui::EndTable();
        }
        Entity picked = entityOf(at.EntityId);
        ImGui::BeginDisabled(picked.IsNull());
        if (ImGui::Button("Select Object")) m_Ctx.Select(picked);
        ImGui::EndDisabled();

        // Every draw, under its pass.
        ImGui::SeparatorText("Draws");
        if (ImGui::BeginTable("##events", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthFixed, 66.0f);
            ImGui::TableHeadersRow();
            const std::string* pass = nullptr;
            for (int i = 0; i < count; ++i)
            {
                const FrameDebugEvent& ev = events[i];
                if (!pass || *pass != ev.Pass)
                {
                    pass = &ev.Pass;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s  -> %s", ev.Pass.c_str(), ev.Target.c_str());
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool selected = i == shown;
                bool after = i > shown;
                if (after) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                char label[16];
                std::snprintf(label, sizeof(label), "%d##draw", i + 1);
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_Event = i == count - 1 ? -1 : i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !entityOf(ev.EntityId).IsNull()) m_Ctx.Select(entityOf(ev.EntityId));
                }
                if (selected && m_ScrollTo)
                {
                    ImGui::SetScrollHereY(0.5f);
                    m_ScrollTo = false;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ev.What.empty() ? "(unnamed mesh)" : ev.What.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(nameOf(ev.EntityId).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", ev.Triangles);
                ImGui::TableNextColumn();
                ImGui::Text("%u", ev.Instances);
                if (after) ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
}
