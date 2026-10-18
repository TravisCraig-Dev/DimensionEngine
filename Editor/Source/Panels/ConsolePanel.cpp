#include "Panels.h"
#include <imgui.h>

namespace Editor
{
    ConsolePanel::ConsolePanel()
    {
        m_SinkId = Log::AddSink([this](const LogEntry& e) {
            std::lock_guard lock(m_Mutex);
            if (m_Collapse && !m_Entries.empty() && m_Entries.back().Text == e.Message && m_Entries.back().Level == e.Level)
                m_Entries.back().Count++;
            else
                m_Entries.push_back({ e.Level, e.Message, e.Time, 1 });
            m_Counts[(int)e.Level]++;
            if (m_Entries.size() > 5000) m_Entries.erase(m_Entries.begin(), m_Entries.begin() + 1000);
            m_ScrollToBottom = true;
        });
    }

    ConsolePanel::~ConsolePanel() { Log::RemoveSink(m_SinkId); }

    void ConsolePanel::Clear()
    {
        std::lock_guard lock(m_Mutex);
        m_Entries.clear();
        for (int& c : m_Counts) c = 0;
    }

    void ConsolePanel::OnImGui(bool* open)
    {
        if (!ImGui::Begin("Console", open))
        {
            ImGui::End();
            return;
        }
        if (ImGui::Button("Clear")) Clear();
        ImGui::SameLine();
        ImGui::Checkbox("Collapse", &m_Collapse);
        ImGui::SameLine();
        ImGui::Checkbox("Clear on Play", &ClearOnPlay);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 300);
        auto toggle = [](const char* label, bool* v, int count, ImVec4 color) {
            ImGui::PushStyleColor(ImGuiCol_Text, *v ? color : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s %d", label, count);
            if (ImGui::Button(buf, ImVec2(90, 0))) *v = !*v;
            ImGui::PopStyleColor();
            ImGui::SameLine();
        };
        {
            std::lock_guard lock(m_Mutex);
            toggle("Info", &m_ShowInfo, m_Counts[1] + m_Counts[0], ImVec4(0.85f, 0.85f, 0.85f, 1));
            toggle("Warn", &m_ShowWarn, m_Counts[2], ImVec4(1.0f, 0.8f, 0.3f, 1));
            toggle("Error", &m_ShowError, m_Counts[3], ImVec4(1.0f, 0.4f, 0.4f, 1));
        }
        ImGui::NewLine();
        ImGui::Separator();

        ImGui::BeginChild("Log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard lock(m_Mutex);
            ImGuiListClipper clipper;
            std::vector<const Entry*> visible;
            for (auto& e : m_Entries)
            {
                bool show = (e.Level <= LogLevel::Info && m_ShowInfo) || (e.Level == LogLevel::Warn && m_ShowWarn) ||
                            (e.Level == LogLevel::Error && m_ShowError);
                if (show) visible.push_back(&e);
            }
            clipper.Begin((int)visible.size());
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const Entry& e = *visible[i];
                    ImVec4 color = e.Level == LogLevel::Error ? ImVec4(1.0f, 0.45f, 0.45f, 1)
                                 : e.Level == LogLevel::Warn  ? ImVec4(1.0f, 0.8f, 0.35f, 1)
                                 : e.Level == LogLevel::Trace ? ImVec4(0.55f, 0.55f, 0.55f, 1)
                                                              : ImVec4(0.85f, 0.85f, 0.85f, 1);
                    ImGui::TextDisabled("[%7.2f]", e.Time);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextUnformatted(e.Text.c_str());
                    ImGui::PopStyleColor();
                    if (e.Count > 1)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(x%d)", e.Count);
                    }
                }
            if (m_ScrollToBottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
            m_ScrollToBottom = false;
        }
        ImGui::EndChild();
        ImGui::End();
    }
}
