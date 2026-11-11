#include "Panels.h"
#include <Dimension/Scripting/CSharpRuntime.h>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace Editor
{
    void TestRunnerPanel::Refresh()
    {
        // The tests the loaded scripts have, keeping the results of those still there.
        std::vector<Test> found;
        try
        {
            for (const auto& t : nlohmann::json::parse(CSharpRuntime::RunTests(false)))
            {
                Test test;
                test.Name = t.value("name", std::string());
                for (const Test& old : m_Tests)
                    if (old.Name == test.Name) test = old;
                found.push_back(test);
            }
        }
        catch (const std::exception&)
        {
        }
        m_Tests = std::move(found);
    }

    void TestRunnerPanel::Run(const std::string& filter)
    {
        Refresh();
        try
        {
            for (const auto& r : nlohmann::json::parse(CSharpRuntime::RunTests(true, filter)))
                for (Test& t : m_Tests)
                    if (t.Name == r.value("name", std::string()))
                    {
                        t.State = r.value("passed", false) ? 1 : 2;
                        t.Milliseconds = r.value("ms", 0.0f);
                        t.Message = r.value("message", std::string());
                    }
        }
        catch (const std::exception&)
        {
        }
        int passed = 0, failed = 0;
        for (const Test& t : m_Tests) passed += t.State == 1, failed += t.State == 2;
        if (failed) Log::Warn("Tests: {} passed, {} failed", passed, failed);
        else Log::Info("Tests: {} passed", passed);
    }

    void TestRunnerPanel::OnImGui(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Test Runner", open))
        {
            ImGui::End();
            return;
        }
        if (!CSharpRuntime::IsAvailable())
        {
            ImGui::TextDisabled("C# scripting is not available.");
            ImGui::End();
            return;
        }
        if (!m_Listed || m_ScriptsPath != CSharpRuntime::GetScriptsPath())
        {
            m_Listed = true;
            m_ScriptsPath = CSharpRuntime::GetScriptsPath();
            Refresh();
        }
        if (m_RunRequested && !CSharpRuntime::GetScriptsPath().empty() && ImGui::GetFrameCount() >= 50) // (automation: --run-tests, once loaded)
        {
            m_RunRequested = false;
            Run("");
        }
        bool playing = m_Ctx.IsPlaying();
        ImGui::BeginDisabled(playing);
        if (ImGui::Button("Run All")) Run("");
        ImGui::SameLine();
        int failedCount = 0;
        for (const Test& t : m_Tests) failedCount += t.State == 2;
        ImGui::BeginDisabled(failedCount == 0);
        if (ImGui::Button("Run Failed"))
            for (const Test& t : std::vector<Test>(m_Tests))
                if (t.State == 2) Run(t.Name);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) Refresh();
        ImGui::SameLine();
        int passed = 0, failed = 0, notRun = 0;
        for (const Test& t : m_Tests) (t.State == 1 ? passed : t.State == 2 ? failed : notRun)++;
        ImGui::TextDisabled("%zu tests: %d passed, %d failed, %d not run", m_Tests.size(), passed, failed, notRun);
        if (m_Tests.empty())
            ImGui::TextDisabled("No tests: mark public methods without parameters with [Test] (using Dimension; Assert checks).");
        else if (ImGui::BeginTable("##tests", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Test");
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (const Test& t : m_Tests)
            {
                ImGui::PushID(t.Name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImVec4 color = t.State == 1 ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : t.State == 2 ? ImVec4(0.95f, 0.4f, 0.35f, 1.0f) : ImVec4(0.6f, 0.6f, 0.65f, 1.0f);
                ImGui::TextColored(color, "%s", t.Name.c_str()); // (green passed, red failed, grey not run)
                if (t.State == 2 && !t.Message.empty())
                {
                    ImGui::Indent();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.55f, 1.0f), "%s", t.Message.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Unindent();
                }
                ImGui::TableNextColumn();
                if (t.State) ImGui::TextDisabled("%.1f ms", t.Milliseconds);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(playing);
                if (ImGui::SmallButton("Run")) Run(t.Name);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
}
