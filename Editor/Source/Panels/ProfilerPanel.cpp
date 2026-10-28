#include "Panels.h"
#include <Dimension/Core/Profiler.h>
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>

namespace Editor
{
    namespace
    {
        ImU32 SectionColor(const char* name)
        {
            // A steady color per section name.
            static const ImU32 palette[] = { IM_COL32(86, 156, 214, 255), IM_COL32(220, 160, 70, 255), IM_COL32(120, 190, 110, 255),
                                             IM_COL32(200, 100, 110, 255), IM_COL32(170, 130, 210, 255), IM_COL32(90, 190, 190, 255),
                                             IM_COL32(210, 200, 90, 255), IM_COL32(150, 150, 150, 255) };
            uint32_t h = 2166136261u;
            for (const char* c = name; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
            return palette[h % (sizeof(palette) / sizeof(palette[0]))];
        }
    }

    // Window > Profiler (like Unity's CPU Profiler): the last few seconds of frames as stacked bars of their top-level
    // sections (Update, Render, ...; the rest of the frame grey), and for the frame picked (the newest unless one is
    // clicked) its sections nested, with milliseconds, share of the frame and calls, and its counters.
    void ProfilerPanel::OnImGui(bool* open)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Profiler", open))
        {
            ImGui::End();
            return;
        }
        const auto& history = Profiler::History();
        bool paused = Profiler::IsPaused();
        if (ImGui::Button(paused ? "Record" : "Pause")) Profiler::SetPaused(!paused);
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            Profiler::Clear();
            m_Picked = -1;
        }
        if (history.empty())
        {
            ImGui::TextDisabled("No frames recorded yet.");
            ImGui::End();
            return;
        }
        std::vector<float> times;
        for (const auto& f : history) times.push_back(f.FrameMs);
        std::sort(times.begin(), times.end());
        float median = times[times.size() / 2], worst = times.back(), typicalWorst = times[std::min(times.size() - 1, times.size() * 95 / 100)];
        ImGui::SameLine();
        ImGui::Text("  %zu frames: %.2f ms median (%.0f fps), %.2f ms slowest", history.size(), median, 1000.0f / std::max(median, 1e-3f), worst);

        // The graph: a bar per frame, scaled to the slower frames (at least 33 ms; a rare hitch such as loading runs off
        // the top), lines at 16.7 and 33.3 ms.
        float width = ImGui::GetContentRegionAvail().x, height = 150.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, { origin.x + width, origin.y + height }, IM_COL32(25, 25, 28, 255));
        float scale = std::max(typicalWorst * 1.25f, 33.4f);
        float barWidth = width / (float)Profiler::kHistory;
        size_t first = Profiler::kHistory - history.size();
        for (size_t i = 0; i < history.size(); ++i)
        {
            const auto& f = history[i];
            float x = origin.x + (first + i) * barWidth, bottom = origin.y + height, used = 0.0f;
            draw->PushClipRect(origin, { origin.x + width, origin.y + height }, true);
            for (const auto& s : f.Sections)
            {
                if (s.Depth != 0) continue;
                float h = s.Ms / scale * height;
                draw->AddRectFilled({ x, bottom - h }, { x + std::max(barWidth - 0.5f, 1.0f), bottom }, SectionColor(s.Name));
                bottom -= h;
                used += s.Ms;
            }
            float rest = std::max(f.FrameMs - used, 0.0f) / scale * height;
            draw->AddRectFilled({ x, bottom - rest }, { x + std::max(barWidth - 0.5f, 1.0f), bottom }, IM_COL32(90, 90, 95, 255));
            draw->PopClipRect();
        }
        for (float ms : { 16.7f, 33.3f })
        {
            float y = origin.y + height - ms / scale * height;
            draw->AddLine({ origin.x, y }, { origin.x + width, y }, IM_COL32(255, 255, 255, 60));
            draw->AddText({ origin.x + 4.0f, y + 2.0f }, IM_COL32(255, 255, 255, 120), ms < 20.0f ? "16.7 ms (60 fps)" : "33.3 ms (30 fps)");
        }
        ImGui::InvisibleButton("##graph", { width, height });
        int newest = (int)history.size() - 1;
        if (ImGui::IsItemHovered())
        {
            int hovered = (int)((ImGui::GetIO().MousePos.x - origin.x) / barWidth) - (int)first;
            if (hovered >= 0 && hovered <= newest)
            {
                ImGui::SetTooltip("Frame %d: %.2f ms", hovered - newest, history[hovered].FrameMs);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    m_Picked = hovered;
                    Profiler::SetPaused(true); // keep it still to look at
                }
            }
        }
        int picked = paused && m_Picked >= 0 && m_Picked <= newest ? m_Picked : newest;
        float pickX = origin.x + (first + picked + 0.5f) * barWidth;
        draw->AddLine({ pickX, origin.y }, { pickX, origin.y + height }, IM_COL32(255, 255, 255, 200));

        // The picked frame.
        const auto& f = history[picked];
        ImGui::Text("Frame %d: %.2f ms", picked - newest, f.FrameMs);
        for (const auto& [name, value] : f.Counters)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("  %s %.0f", name.c_str(), value);
        }
        // CPU sections on the left, GPU spans (render passes) on the right.
        float half = ImGui::GetContentRegionAvail().x * 0.5f - 4.0f;
        ImGui::BeginChild("##cpu", ImVec2(half, 0.0f));
        ImGui::TextDisabled("CPU");
        if (ImGui::BeginTable("##sections", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("ms");
            ImGui::TableSetupColumn("% of frame");
            ImGui::TableSetupColumn("Calls");
            ImGui::TableHeadersRow();
            for (const auto& s : f.Sections)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (s.Depth > 0) ImGui::Indent(s.Depth * 14.0f); // (0 would indent by the default step)
                ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(SectionColor(s.Name)), ImGuiColorEditFlags_NoTooltip, ImVec2(10.0f, 10.0f));
                ImGui::SameLine();
                ImGui::TextUnformatted(s.Name);
                if (s.Depth > 0) ImGui::Unindent(s.Depth * 14.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", s.Ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", f.FrameMs > 0.0f ? s.Ms / f.FrameMs * 100.0f : 0.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%d", s.Calls);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##gpu", ImVec2(0.0f, 0.0f));
        float gpuFrame = f.Gpu.empty() ? 0.0f : f.Gpu.front().Ms;
        ImGui::TextDisabled("GPU %.2f ms (a frame or two earlier: it runs behind)", gpuFrame);
        if (f.Gpu.empty()) ImGui::TextDisabled("No GPU timings (timestamps not supported, or not yet back).");
        else if (ImGui::BeginTable("##gpuSections", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("ms");
            ImGui::TableSetupColumn("% of GPU frame");
            ImGui::TableHeadersRow();
            for (const auto& s : f.Gpu)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (s.Depth > 0) ImGui::Indent(s.Depth * 14.0f);
                ImGui::TextUnformatted(s.Name);
                if (s.Depth > 0) ImGui::Unindent(s.Depth * 14.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", s.Ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", gpuFrame > 0.0f ? s.Ms / gpuFrame * 100.0f : 0.0f);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::End();
    }
}
