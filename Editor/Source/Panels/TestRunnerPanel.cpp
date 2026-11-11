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
                test.Play = t.value("play", false);
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
        for (const Test& t : m_Tests)
            if (!t.Play) passed += t.State == 1, failed += t.State == 2;
        if (failed) Log::Warn("Edit mode tests: {} passed, {} failed", passed, failed);
        else Log::Info("Edit mode tests: {} passed", passed);
    }

    void TestRunnerPanel::RunPlay(const std::vector<std::string>& tests)
    {
        if (tests.empty() || !m_PlayQueue.empty()) return;
        m_PlayQueue = tests;
        m_PlayTotal = (int)tests.size();
        for (Test& t : m_Tests)
            if (std::find(tests.begin(), tests.end(), t.Name) != tests.end()) t.State = 0, t.Message.clear();
        m_Ctx.RequestPlayTest = m_PlayQueue.front();
    }

    void TestRunnerPanel::Poll()
    {
        if (m_RunRequested && !CSharpRuntime::GetScriptsPath().empty() && ImGui::GetFrameCount() >= 50) // (automation: --run-tests, once loaded)
        {
            m_RunRequested = false;
            Run("");
            std::vector<std::string> play;
            for (const Test& t : m_Tests)
                if (t.Play) play.push_back(t.Name);
            RunPlay(play);
        }
        if (m_PlayQueue.empty()) return;
        for (Test& t : m_Tests)
            if (t.Name == m_PlayQueue.front() && t.State == 0 && m_Ctx.RunningPlayTest == t.Name) t.State = 3;
        if (!m_Ctx.PlayTestResult) return;
        std::string name = m_PlayQueue.front();
        bool stopped = false;
        try
        {
            auto r = nlohmann::json::parse(*m_Ctx.PlayTestResult);
            for (Test& t : m_Tests)
                if (t.Name == name)
                {
                    t.State = r.value("passed", false) ? 1 : 2;
                    t.Milliseconds = r.value("ms", 0.0f);
                    t.Message = r.value("message", std::string());
                    t.Frames = r.value("frames", 0);
                    stopped = t.Message.starts_with("Stopped");
                    if (t.State == 2) Log::Error("Play mode test {} failed: {}", name, t.Message);
                }
        }
        catch (const std::exception&)
        {
        }
        m_Ctx.PlayTestResult.reset();
        m_PlayQueue.erase(m_PlayQueue.begin());
        if (stopped) m_PlayQueue.clear(); // Stop pressed: the rest are not run
        if (!m_PlayQueue.empty())
        {
            m_Ctx.RequestPlayTest = m_PlayQueue.front();
            return;
        }
        int passed = 0, failed = 0;
        for (const Test& t : m_Tests)
            if (t.Play) passed += t.State == 1, failed += t.State == 2;
        if (failed) Log::Warn("Play mode tests: {} passed, {} failed", passed, failed);
        else Log::Info("Play mode tests: {} passed", passed);
    }

    void TestRunnerPanel::DrawTests(bool play)
    {
        bool busy = !m_PlayQueue.empty();
        bool editDisabled = m_Ctx.IsPlaying() || busy; // edit-mode tests run outside play mode
        int passed = 0, failed = 0, notRun = 0, count = 0;
        for (const Test& t : m_Tests)
            if (t.Play == play) ++count, (t.State == 1 ? passed : t.State == 2 ? failed : notRun)++;

        ImGui::BeginDisabled(play ? busy : editDisabled);
        if (ImGui::Button("Run All"))
        {
            if (play)
            {
                std::vector<std::string> names;
                for (const Test& t : m_Tests)
                    if (t.Play) names.push_back(t.Name);
                RunPlay(names);
            }
            else Run("");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(failed == 0);
        if (ImGui::Button("Run Failed"))
        {
            std::vector<std::string> names;
            for (const Test& t : m_Tests)
                if (t.Play == play && t.State == 2) names.push_back(t.Name);
            if (play) RunPlay(names);
            else
                for (const std::string& n : names) Run(n);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Refresh")) Refresh();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (play && busy)
            ImGui::TextDisabled("Running %d of %d (Stop ends the run)", m_PlayTotal - (int)m_PlayQueue.size() + 1, m_PlayTotal);
        else ImGui::TextDisabled("%d tests: %d passed, %d failed, %d not run", count, passed, failed, notRun);

        if (count == 0)
        {
            ImGui::PushTextWrapPos(0.0f);
            if (play)
                ImGui::TextDisabled("No play mode tests: mark public methods returning IEnumerator with [PlayTest]. Each runs in play mode on an "
                                    "empty scene, resumed once a frame: yield return null for a frame, WaitForSeconds, WaitUntil, a "
                                    "SceneManager.LoadSceneAsync operation; Assert checks.");
            else ImGui::TextDisabled("No tests: mark public methods without parameters with [Test] (using Dimension; Assert checks).");
            ImGui::PopTextWrapPos();
            return;
        }
        if (!ImGui::BeginTable("##tests", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) return;
        ImGui::TableSetupColumn("Test");
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, play ? 130.0f : 70.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const Test& t : m_Tests)
        {
            if (t.Play != play) continue;
            ImGui::PushID(t.Name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImVec4 color = t.State == 1   ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f)
                           : t.State == 2 ? ImVec4(0.95f, 0.4f, 0.35f, 1.0f)
                           : t.State == 3 ? ImVec4(0.95f, 0.8f, 0.35f, 1.0f)
                                          : ImVec4(0.6f, 0.6f, 0.65f, 1.0f);
            ImGui::TextColored(color, "%s", t.Name.c_str()); // (green passed, red failed, yellow running, grey not run)
            if (t.State == 2 && !t.Message.empty())
            {
                ImGui::Indent();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.55f, 1.0f), "%s", t.Message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent();
            }
            ImGui::TableNextColumn();
            if (t.State == 3) ImGui::TextDisabled("running");
            else if (t.State && play) ImGui::TextDisabled("%.0f ms, %d fr", t.Milliseconds, t.Frames);
            else if (t.State) ImGui::TextDisabled("%.1f ms", t.Milliseconds);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(play ? busy : editDisabled);
            if (ImGui::SmallButton("Run"))
            {
                if (play) RunPlay({ t.Name });
                else Run(t.Name);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
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
            if (m_PlayQueue.empty()) Refresh();
        }
        if (ImGui::BeginTabBar("##modes"))
        {
            // The Play Mode tab comes forward while its tests run.
            bool playing = !m_PlayQueue.empty();
            if (ImGui::BeginTabItem("Edit Mode"))
            {
                DrawTests(false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Play Mode", nullptr, playing && m_PlayTotal == (int)m_PlayQueue.size() ? ImGuiTabItemFlags_SetSelected : 0))
            {
                DrawTests(true);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
}
