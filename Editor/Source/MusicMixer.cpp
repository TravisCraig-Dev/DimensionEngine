// Music Mode's Mixer: the Master and insert tracks as strips (meter, fader, pan, mute, routing), and the selected
// track's effect chain with its settings. Also automation clips made from any automatable control.
#include "MusicMode.h"
#include "MusicModeUI.h"
#include "EditorUI.h"
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Music;
    using namespace Editor::MusicUI;

    void MusicMode::AutomationMenu(const ParamRef& ref)
    {
        if (ImGui::BeginPopupContextItem())
        {
            ImGui::TextDisabled("%s", m_Song.ParamName(ref).c_str());
            if (ImGui::MenuItem("Create Automation Clip")) CreateAutomation(ref);
            ImGui::EndPopup();
        }
    }

    void MusicMode::CreateAutomation(const ParamRef& ref)
    {
        float* value = m_Song.Param(ref);
        if (!value) return;
        Record();
        // At the song position (or the start), on the first Playlist track free there, flat at the current value.
        AutomationClip a;
        a.Target = ref;
        a.Start = m_SongMode && m_Player ? std::max(0, (int)(m_Player->GetTick() / m_Song.BarTicks()) * m_Song.BarTicks()) : 0;
        a.Length = m_Song.BarTicks() * 4;
        float n = m_Song.ToNormalized(ref, *value);
        a.Points = { { 0, n }, { a.Length, n } };
        for (int track = 0;; ++track)
        {
            bool busy = false;
            for (const auto& c : m_Song.Playlist)
                busy |= c.Track == track && c.Start < a.Start + a.Length && c.Start + m_Song.ClipTicks(c) > a.Start;
            for (const auto& c : m_Song.Automation) busy |= c.Track == track && c.Start < a.Start + a.Length && c.Start + c.Length > a.Start;
            if (!busy)
            {
                a.Track = track;
                break;
            }
        }
        m_Song.Automation.push_back(a);
        m_SelectedAuto = (int)m_Song.Automation.size() - 1;
        m_SelectedClip = -1;
        Changed();
        m_View = View::Playlist;
        m_Status = "Automation clip for " + m_Song.ParamName(ref) + " (Song mode plays it)";
        m_StatusTime = 4.0f;
    }

    void MusicMode::DrawMixer(const ImVec2& size)
    {
        if (m_SelectedMixer >= (int)m_Song.Mixer.size()) m_SelectedMixer = 0;
        float effectsW = std::clamp(size.x * 0.3f, 300.0f, 420.0f);
        ImGui::BeginChild("##Mixer", ImVec2(size.x - effectsW - ImGui::GetStyle().ItemSpacing.x, size.y), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Mixer");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginDisabled((int)m_Song.Mixer.size() >= SongPlayer::kMaxMixer);
        if (ImGui::Button("+ Insert"))
        {
            Record();
            MixerTrack m;
            m.Name = "Insert " + std::to_string(m_Song.Mixer.size());
            m_Song.Mixer.push_back(m);
            m_SelectedMixer = (int)m_Song.Mixer.size() - 1;
            m_SelectedEffect = -1;
            Changed();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("Channels pick their track in their settings. Right-click a control to automate it.");

        // Meters fall back between peaks.
        m_MixerMeters.resize(m_Song.Mixer.size(), 0.0f);
        for (size_t i = 0; i < m_MixerMeters.size(); ++i)
            m_MixerMeters[i] = std::max(m_Player ? m_Player->TakeMixerPeak((int)i) : 0.0f, m_MixerMeters[i] * 0.88f);

        const float stripW = 86.0f;
        float stripH = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize - 4.0f;
        int removeTrack = -1;
        for (int t = 0; t < (int)m_Song.Mixer.size(); ++t)
        {
            MixerTrack& m = m_Song.Mixer[t];
            ImGui::PushID(t);
            if (t > 0) ImGui::SameLine(0.0f, t == 1 ? 14.0f : 4.0f);
            bool selected = t == m_SelectedMixer;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? UI::AccentBackground(0.3f) : ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
            ImGui::BeginChild("##strip", ImVec2(stripW, stripH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleColor();
            // Name (click selects, double-click renames, right-click for more).
            if (m_RenamingMixer == t)
            {
                ImGui::SetNextItemWidth(-1);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) ||
                    (!ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                {
                    if (!m_RenameBuffer.empty() && m_RenameBuffer != m.Name)
                    {
                        Record();
                        m.Name = m_RenameBuffer;
                        Changed();
                    }
                    m_RenamingMixer = -1;
                }
            }
            else
            {
                std::string name = m_Song.MixerTrackName(t);
                if (ImGui::Selectable(name.c_str(), selected, 0, ImVec2(0, ImGui::GetFrameHeight())))
                {
                    if (m_SelectedMixer != t) m_SelectedEffect = -1;
                    m_SelectedMixer = t;
                }
                if (t > 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_RenamingMixer = t, m_RenameBuffer = name;
                if (ImGui::BeginPopupContextItem("##stripMenu"))
                {
                    if (t > 0 && ImGui::MenuItem("Rename")) m_RenamingMixer = t, m_RenameBuffer = name;
                    if (ImGui::MenuItem("Create Volume Automation")) CreateAutomation({ ParamKind::MixerVolume, t });
                    if (ImGui::MenuItem("Create Pan Automation")) CreateAutomation({ ParamKind::MixerPan, t });
                    if (t > 0 && ImGui::MenuItem("Delete")) removeTrack = t;
                    ImGui::EndPopup();
                }
            }
            // Pan knob, mute.
            ImGui::SetCursorPosX((stripW - 26.0f) * 0.5f);
            Knob("##pan", &m.Pan, -1.0f, 1.0f, 0.0f, 26.0f, "Pan %.2f");
            AutomationMenu({ ParamKind::MixerPan, t });
            ImGui::SetCursorPosX(6.0f);
            if (m.Mute) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.25f, 0.2f, 1.0f));
            if (ImGui::Button(m.Mute ? "Muted" : "Mute", ImVec2(stripW - 12.0f, 0)))
            {
                Record();
                m.Mute = !m.Mute;
                Changed();
            }
            if (m.Mute) ImGui::PopStyleColor();
            // Fader and meter side by side.
            float faderH = stripH - ImGui::GetCursorPosY() - ImGui::GetFrameHeightWithSpacing() * 2.0f - 8.0f;
            ImGui::SetCursorPosX(10.0f);
            float vol = m.Volume;
            if (Edited(ImGui::VSliderFloat("##fader", ImVec2(28.0f, std::max(40.0f, faderH)), &vol, 0.0f, 1.25f, "")))
            {
                m.Volume = vol;
                Changed();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                Record();
                m.Volume = t == 0 ? 0.8f : 1.0f;
                Changed();
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetTooltip("%.1f dB", 20.0f * std::log10(std::max(m.Volume, 1e-4f)));
            AutomationMenu({ ParamKind::MixerVolume, t });
            ImGui::SameLine(0.0f, 8.0f);
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                float h = std::max(40.0f, faderH);
                ImGui::Dummy(ImVec2(14.0f, h));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                float level = std::min(1.0f, t < (int)m_MixerMeters.size() ? m_MixerMeters[t] : 0.0f);
                dl->AddRectFilled(p, ImVec2(p.x + 14, p.y + h), IM_COL32(25, 25, 28, 255));
                ImU32 col = level >= 1.0f ? IM_COL32(235, 70, 60, 255) : level > 0.7f ? IM_COL32(230, 200, 60, 255) : IM_COL32(90, 200, 110, 255);
                dl->AddRectFilled(ImVec2(p.x + 2, p.y + h * (1.0f - level)), ImVec2(p.x + 12, p.y + h), col);
            }
            ImGui::Text("%+.1f dB", 20.0f * std::log10(std::max(m.Volume, 1e-4f)));
            // Output: the Master or a later track.
            ImGui::SetNextItemWidth(-1);
            if (t > 0)
            {
                std::string out = "> " + m_Song.MixerTrackName(m.Output);
                if (ImGui::BeginCombo("##output", out.c_str(), ImGuiComboFlags_NoArrowButton))
                {
                    for (int o = 0; o < (int)m_Song.Mixer.size(); ++o)
                    {
                        if (o != 0 && o <= t) continue;
                        if (ImGui::Selectable(m_Song.MixerTrackName(o).c_str(), m.Output == o))
                        {
                            Record();
                            m.Output = o;
                            Changed();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Where the track goes: the Master, or a later insert (a bus)");
            }
            else ImGui::TextDisabled("Output");
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_SelectedMixer != t)
            {
                m_SelectedMixer = t;
                m_SelectedEffect = -1;
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
        if (removeTrack > 0)
        {
            Record();
            m_Song.RemoveMixerTrack(removeTrack);
            m_SelectedMixer = std::min(m_SelectedMixer, (int)m_Song.Mixer.size() - 1);
            m_SelectedEffect = -1;
            Changed();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        DrawEffectPanel(ImVec2(effectsW, size.y));
    }

    void MusicMode::DrawEffectPanel(const ImVec2& size)
    {
        ImGui::BeginChild("##Effects", size, ImGuiChildFlags_Borders);
        const int t = m_SelectedMixer;
        MixerTrack& m = m_Song.Mixer[t];
        ImGui::TextColored(UI::AccentText(), "%s", m_Song.MixerTrackName(t).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("effects (top to bottom)");
        // The routed channels.
        std::string routed;
        for (const Channel& c : m_Song.Channels)
            if (c.Insert == t) routed += (routed.empty() ? "" : ", ") + c.Name;
        if (!routed.empty()) ImGui::TextDisabled("Channels: %s", routed.c_str());
        ImGui::Separator();

        // The chain: enable, name (select), move, remove.
        int remove = -1, moveUp = -1;
        for (int i = 0; i < (int)m.Effects.size(); ++i)
        {
            Effect& e = m.Effects[i];
            ImGui::PushID(i);
            if (Edited(ImGui::Checkbox("##on", &e.Enabled))) Changed();
            ImGui::SetItemTooltip("On / bypassed");
            ImGui::SameLine();
            std::string label = std::to_string(i + 1) + ". " + (e.Type == EffectType::Plugin ? e.Plugin.Name : std::string(EffectTypeName(e.Type)));
            if (ImGui::Selectable(label.c_str(), i == m_SelectedEffect, 0, ImVec2(ImGui::GetContentRegionAvail().x - 54.0f, 0))) m_SelectedEffect = i;
            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) moveUp = i;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::PopID();
        }
        if (moveUp > 0)
        {
            Record();
            std::swap(m.Effects[moveUp], m.Effects[moveUp - 1]);
            for (AutomationClip& a : m_Song.Automation)
                if ((a.Target.Kind == ParamKind::EffectParam || a.Target.Kind == ParamKind::EffectMix) && a.Target.Index == t)
                {
                    if (a.Target.Slot == moveUp) a.Target.Slot = moveUp - 1;
                    else if (a.Target.Slot == moveUp - 1) a.Target.Slot = moveUp;
                }
            if (m_SelectedEffect == moveUp) m_SelectedEffect = moveUp - 1;
            Changed();
        }
        if (remove >= 0)
        {
            Record();
            m_Song.RemoveEffect(t, remove);
            m_SelectedEffect = std::min(m_SelectedEffect, (int)m_Song.Mixer[t].Effects.size() - 1);
            Changed();
        }
        ImGui::BeginDisabled((int)m.Effects.size() >= kMaxEffects);
        if (ImGui::Button("+ Effect")) ImGui::OpenPopup("AddEffect");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("AddEffect"))
        {
            for (int i = 0; i < (int)EffectType::Count; ++i)
                if (i != (int)EffectType::Plugin && ImGui::MenuItem(EffectTypeName((EffectType)i)))
                {
                    Record();
                    m.Effects.push_back(Effect::Make((EffectType)i));
                    m_SelectedEffect = (int)m.Effects.size() - 1;
                    Changed();
                }
            ImGui::Separator();
            if (ImGui::BeginMenu("Plug-in"))
            {
                PluginRef picked;
                if (PluginMenu(false, picked))
                {
                    Record();
                    Effect e = Effect::Make(EffectType::Plugin);
                    e.Plugin = picked;
                    m.Effects.push_back(e);
                    m_SelectedEffect = (int)m.Effects.size() - 1;
                    Changed();
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        // The selected effect's settings (right-click one to automate it).
        if (m_SelectedEffect >= 0 && m_SelectedEffect < (int)m.Effects.size())
        {
            Effect& e = m.Effects[m_SelectedEffect];
            const EffectInfo& info = GetEffectInfo(e.Type);
            if (e.Type == EffectType::Plugin)
            {
                // A plug-in: its mix here, its own settings in its panel (and editor window).
                ImGui::SeparatorText("Plug-in");
                if (UI::BeginProperties("##pluginMix"))
                {
                    UI::PropertyLabel("Mix", "Dry to wet");
                    if (Edited(ImGui::SliderFloat("##mix", &e.Mix, 0.0f, 1.0f, "%.2f"))) Changed();
                    AutomationMenu({ ParamKind::EffectMix, t, m_SelectedEffect });
                    UI::EndProperties();
                }
                DrawPluginPanel(e.Plugin, t, m_SelectedEffect, ImGui::GetContentRegionAvail().y);
                ImGui::EndChild();
                return;
            }
            ImGui::SeparatorText(EffectTypeName(e.Type));
            if (UI::BeginProperties("##fx"))
            {
                for (int p = 0; p < info.ParamCount; ++p)
                {
                    const EffectParamInfo& pi = info.Params[p];
                    UI::PropertyLabel(pi.Name);
                    ImGui::PushID(p);
                    bool changed;
                    if (e.Type == EffectType::Filter && p == 0)
                    {
                        int mode = (int)std::lround(e.P[0]);
                        const char* modes[] = { "Low Pass", "High Pass", "Band Pass" };
                        changed = ImGui::Combo("##mode", &mode, modes, 3);
                        if (changed) e.P[0] = (float)mode;
                    }
                    else if (e.Type == EffectType::Delay && p == 2)
                    {
                        bool on = e.P[2] >= 0.5f;
                        changed = ImGui::Checkbox("##pp", &on);
                        if (changed) e.P[2] = on ? 1.0f : 0.0f;
                    }
                    else if (e.Type == EffectType::Delay && p == 0)
                    {
                        int steps = (int)std::lround(e.P[0]);
                        changed = ImGui::SliderInt("##time", &steps, 1, 16, "%d steps");
                        if (changed) e.P[0] = (float)steps;
                    }
                    else changed = ImGui::SliderFloat("##v", &e.P[p], pi.Min, pi.Max, pi.Format, pi.Log ? ImGuiSliderFlags_Logarithmic : 0);
                    if (Edited(changed)) Changed();
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && e.P[p] != pi.Default && !ImGui::IsItemActive())
                    {
                        Record();
                        e.P[p] = pi.Default;
                        Changed();
                    }
                    AutomationMenu({ ParamKind::EffectParam, t, m_SelectedEffect, p });
                    ImGui::PopID();
                }
                UI::PropertyLabel(info.Send ? "Wet" : "Mix", info.Send ? "How much of the effect is added to the sound" : "Dry to wet");
                if (Edited(ImGui::SliderFloat("##mix", &e.Mix, 0.0f, 1.0f, "%.2f"))) Changed();
                AutomationMenu({ ParamKind::EffectMix, t, m_SelectedEffect });
                UI::EndProperties();
            }
            ImGui::TextDisabled("Double-click a setting to reset it.");
        }
        else if (!m.Effects.empty()) ImGui::TextDisabled("Select an effect to see its settings.");
        ImGui::EndChild();
    }
}
