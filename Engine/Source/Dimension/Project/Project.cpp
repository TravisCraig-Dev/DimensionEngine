#include "Dimension/Project/Project.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace Dimension
{
    namespace
    {
        ProjectConfig s_Config;
        fs::path s_Directory;
        bool s_Open = false;
    }

    bool Project::Open(const fs::path& directory)
    {
        std::error_code ec;
        fs::path dir = fs::weakly_canonical(directory, ec);
        fs::create_directories(dir / "Assets", ec);
        s_Directory = dir;
        s_Config = ProjectConfig();
        s_Config.Name = dir.filename().string();

        fs::path file = dir / kFileName;
        if (fs::exists(file))
        {
            try
            {
                auto j = nlohmann::json::parse(std::ifstream(file));
                s_Config.Name = j.value("name", s_Config.Name);
                s_Config.StartScene = Guid(j.value("startScene", uint64_t(0)));
                s_Config.WindowWidth = j.value("windowWidth", s_Config.WindowWidth);
                s_Config.WindowHeight = j.value("windowHeight", s_Config.WindowHeight);
                s_Config.GameModule = j.value("gameModule", s_Config.GameModule);
                for (auto& h : j.value("buildScenes", nlohmann::json::array())) s_Config.BuildScenes.push_back(Guid(h.get<uint64_t>()));
                s_Config.Fullscreen = j.value("fullscreen", false);
                s_Config.VSync = j.value("vsync", true);
                s_Config.BuildFolder = j.value("buildFolder", std::string());
                s_Config.Icon = Guid(j.value("icon", uint64_t(0)));
                s_Config.StripUnusedAssets = j.value("stripUnusedAssets", true);
                if (j.contains("audioMixer") && j["audioMixer"].is_array() && !j["audioMixer"].empty())
                {
                    s_Config.MixerGroups.clear();
                    for (auto& g : j["audioMixer"])
                    {
                        AudioMixerGroup group;
                        group.Name = g.value("name", group.Name);
                        group.Parent = g.value("parent", 0);
                        group.VolumeDb = g.value("volumeDb", 0.0f);
                        group.Mute = g.value("mute", false);
                        group.Solo = g.value("solo", false);
                        group.LowPassHz = g.value("lowPassHz", 0.0f);
                        group.Echo = g.value("echo", false);
                        group.EchoDelayMs = g.value("echoDelayMs", group.EchoDelayMs);
                        group.EchoDecay = g.value("echoDecay", group.EchoDecay);
                        group.EchoWet = g.value("echoWet", group.EchoWet);
                        group.Reverb = g.value("reverb", false);
                        group.ReverbRoomSize = g.value("reverbRoomSize", group.ReverbRoomSize);
                        group.ReverbDamping = g.value("reverbDamping", group.ReverbDamping);
                        group.ReverbWet = g.value("reverbWet", group.ReverbWet);
                        group.Convolution = g.value("convolution", false);
                        group.ConvolutionImpulse = g.value("convolutionImpulse", uint64_t(0));
                        group.ConvolutionWet = g.value("convolutionWet", group.ConvolutionWet);
                        group.SendTarget = g.value("sendTo", -1);
                        group.SendLevelDb = g.value("sendDb", 0.0f);
                        s_Config.MixerGroups.push_back(group);
                    }
                }
                for (auto& sj : j.value("audioSnapshots", nlohmann::json::array()))
                {
                    AudioMixerSnapshot snap;
                    snap.Name = sj.value("name", snap.Name);
                    for (auto& v : sj.value("values", nlohmann::json::array()))
                        snap.Values.push_back({ v.value("group", std::string()), v.value("volumeDb", 0.0f), v.value("lowPassHz", 0.0f) });
                    s_Config.MixerSnapshots.push_back(std::move(snap));
                }
                if (j.contains("physicsLayers") && j["physicsLayers"].is_array())
                    for (size_t i = 0; i < j["physicsLayers"].size() && i < ProjectConfig::kMaxLayers; ++i)
                        s_Config.PhysicsLayers[i] = j["physicsLayers"][i].get<std::string>();
                if (j.contains("navAreas") && j["navAreas"].is_array())
                    for (size_t i = 0; i < j["navAreas"].size() && i < ProjectConfig::kNavAreas; ++i)
                    {
                        s_Config.NavAreaNames[i] = j["navAreas"][i].value("name", s_Config.NavAreaNames[i]);
                        s_Config.NavAreaCosts[i] = j["navAreas"][i].value("cost", s_Config.NavAreaCosts[i]);
                    }
                s_Config.NavPathBudget = j.value("navPathBudget", s_Config.NavPathBudget);
                s_Config.ImportRules.clear();
                for (auto& r : j.value("importRules", nlohmann::json::array()))
                    s_Config.ImportRules.push_back({ r.value("folder", std::string()), (AssetType)r.value("type", 0),
                                                     r.value("settings", nlohmann::json::object()) });
                s_Config.DefaultPresets.clear();
                for (auto& [component, preset] : j.value("defaultPresets", nlohmann::json::object()).items())
                    if (preset.is_number_unsigned()) s_Config.DefaultPresets[component] = AssetHandle(preset.get<uint64_t>());
                if (j.contains("layerCollisionMatrix") && j["layerCollisionMatrix"].is_array())
                    for (size_t i = 0; i < j["layerCollisionMatrix"].size() && i < ProjectConfig::kMaxLayers; ++i)
                        s_Config.LayerCollisionMatrix[i] = j["layerCollisionMatrix"][i].get<uint32_t>();
            }
            catch (const std::exception& e)
            {
                Log::Error("Invalid project file '{}': {}", file.string(), e.what());
                return false;
            }
        }
        s_Open = true;
        AudioMixer::MarkStructureDirty(); // a new project means a new mixer graph
        AssetManager::Init(dir / "Assets");
        if (!fs::exists(file)) Save();
        Log::Info("Opened project '{}' ({})", s_Config.Name, dir.string());
        return true;
    }

    bool Project::Save()
    {
        if (!s_Open) return false;
        nlohmann::json mixer = nlohmann::json::array();
        for (auto& g : s_Config.MixerGroups)
            mixer.push_back({ { "name", g.Name }, { "parent", g.Parent }, { "volumeDb", g.VolumeDb }, { "mute", g.Mute },
                              { "solo", g.Solo }, { "lowPassHz", g.LowPassHz }, { "echo", g.Echo }, { "echoDelayMs", g.EchoDelayMs },
                              { "echoDecay", g.EchoDecay }, { "echoWet", g.EchoWet }, { "reverb", g.Reverb },
                              { "reverbRoomSize", g.ReverbRoomSize }, { "reverbDamping", g.ReverbDamping }, { "reverbWet", g.ReverbWet },
                              { "sendTo", g.SendTarget }, { "sendDb", g.SendLevelDb }, { "convolution", g.Convolution },
                              { "convolutionImpulse", g.ConvolutionImpulse }, { "convolutionWet", g.ConvolutionWet } });
        nlohmann::json buildScenes = nlohmann::json::array();
        for (AssetHandle h : s_Config.BuildScenes) buildScenes.push_back((uint64_t)h);
        nlohmann::json snapshots = nlohmann::json::array();
        for (auto& snap : s_Config.MixerSnapshots)
        {
            nlohmann::json values = nlohmann::json::array();
            for (auto& v : snap.Values) values.push_back({ { "group", v.Group }, { "volumeDb", v.VolumeDb }, { "lowPassHz", v.LowPassHz } });
            snapshots.push_back({ { "name", snap.Name }, { "values", values } });
        }
        nlohmann::json navAreas = nlohmann::json::array();
        for (int i = 0; i < ProjectConfig::kNavAreas; ++i) navAreas.push_back({ { "name", s_Config.NavAreaNames[i] }, { "cost", s_Config.NavAreaCosts[i] } });
        nlohmann::json importRules = nlohmann::json::array();
        for (const ImportRule& r : s_Config.ImportRules)
            importRules.push_back({ { "folder", r.Folder }, { "type", (int)r.Type }, { "settings", r.Settings } });
        nlohmann::json defaultPresets = nlohmann::json::object();
        for (auto& [component, preset] : s_Config.DefaultPresets) defaultPresets[component] = (uint64_t)preset;
        nlohmann::json j{ { "name", s_Config.Name },
                          { "buildScenes", buildScenes },
                          { "audioMixer", mixer },
                          { "audioSnapshots", snapshots },
                          { "startScene", (uint64_t)s_Config.StartScene },
                          { "windowWidth", s_Config.WindowWidth },
                          { "windowHeight", s_Config.WindowHeight },
                          { "gameModule", s_Config.GameModule },
                          { "fullscreen", s_Config.Fullscreen },
                          { "vsync", s_Config.VSync },
                          { "buildFolder", s_Config.BuildFolder },
                          { "icon", (uint64_t)s_Config.Icon },
                          { "stripUnusedAssets", s_Config.StripUnusedAssets },
                          { "physicsLayers", s_Config.PhysicsLayers },
                          { "layerCollisionMatrix", s_Config.LayerCollisionMatrix },
                          { "navAreas", navAreas },
                          { "navPathBudget", s_Config.NavPathBudget },
                          { "defaultPresets", defaultPresets },
                          { "importRules", importRules },
                          { "engineVersion", "0.1.0" } };
        std::ofstream out(s_Directory / kFileName);
        out << j.dump(2);
        return (bool)out;
    }

    bool Project::IsOpen() { return s_Open; }
    ProjectConfig& Project::GetConfig() { return s_Config; }
    const fs::path& Project::GetDirectory() { return s_Directory; }
    fs::path Project::GetAssetsDirectory() { return s_Directory / "Assets"; }
}
