#include "Dimension/Project/Project.h"
#include "Dimension/Asset/AssetPack.h"
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
        fs::path s_AssetsDirectory; // Assets, or a packed game's unpacked copy
        bool s_Open = false;
    }

    bool Project::Open(const fs::path& directory)
    {
        std::error_code ec;
        fs::path dir = fs::weakly_canonical(directory, ec);
        s_Directory = dir;
        s_AssetsDirectory = dir / "Assets";
        bool packed = !fs::exists(dir / "Assets") && fs::exists(dir / "Assets.pak");
        if (!packed) fs::create_directories(dir / "Assets", ec);
        s_Config = ProjectConfig();
        s_Config.Name = dir.filename().string();

        fs::path file = dir / kFileName;
        if (fs::exists(file))
        {
            try
            {
                auto j = nlohmann::json::parse(std::ifstream(file));
                s_Config.Name = j.value("name", s_Config.Name);
                s_Config.Version = j.value("version", std::string("1.0"));
                s_Config.CompanyName = j.value("companyName", std::string());
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
                s_Config.PackAssets = j.value("packAssets", false);
                s_Config.BundleDotNet = j.value("bundleDotNet", true);
                s_Config.AheadOfTimeScripts = j.value("aheadOfTimeScripts", true);
                s_Config.TrimDotNet = j.value("trimDotNet", true);
                s_Config.DefaultQuality = j.value("defaultQuality", 3);
                s_Config.QualityLevels.clear();
                for (auto& q : j.value("quality", nlohmann::json::array()))
                {
                    QualityLevel level;
                    level.Name = q.value("name", std::string("Level"));
                    level.MaxMSAA = q.value("maxMsaa", level.MaxMSAA);
                    level.AmbientOcclusion = q.value("ssao", level.AmbientOcclusion);
                    level.ScreenSpaceReflections = q.value("ssr", level.ScreenSpaceReflections);
                    level.ScreenSpaceGI = q.value("ssgi", level.ScreenSpaceGI);
                    level.Bloom = q.value("bloom", level.Bloom);
                    level.DepthOfField = q.value("depthOfField", level.DepthOfField);
                    level.MotionBlur = q.value("motionBlur", level.MotionBlur);
                    level.CloudShadows = q.value("cloudShadows", level.CloudShadows);
                    level.MaxRaySteps = q.value("maxRaySteps", level.MaxRaySteps);
                    level.MaxGIRays = q.value("maxGiRays", level.MaxGIRays);
                    level.ShadowResolution = q.value("shadowResolution", level.ShadowResolution);
                    level.ShadowDistance = q.value("shadowDistance", level.ShadowDistance);
                    level.RenderScale = q.value("renderScale", level.RenderScale);
                    s_Config.QualityLevels.push_back(level);
                }
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
                if (j.contains("navAgentTypes") && j["navAgentTypes"].is_array())
                {
                    s_Config.NavAgentTypes.clear();
                    for (const auto& t : j["navAgentTypes"])
                    {
                        ProjectConfig::NavAgentType type;
                        type.Name = t.value("name", type.Name);
                        type.Radius = t.value("radius", type.Radius);
                        type.Height = t.value("height", type.Height);
                        type.StepHeight = t.value("stepHeight", type.StepHeight);
                        type.MaxSlope = t.value("maxSlope", type.MaxSlope);
                        s_Config.NavAgentTypes.push_back(type);
                    }
                }
                s_Config.DefaultLanguage = j.value("defaultLanguage", std::string());
                // Input actions: each binding's kind and control code (with its name, for people reading the file).
                s_Config.InputActions.clear();
                s_Config.InputActionsSet = j.contains("inputActions") && j["inputActions"].is_array();
                for (auto& a : j.value("inputActions", nlohmann::json::array()))
                {
                    InputAction action{ a.value("name", std::string()), a.value("axis", false), a.value("deadZone", 0.15f), {} };
                    action.Sensitivity = a.value("sensitivity", 0.0f);
                    action.Gravity = a.value("gravity", 0.0f);
                    action.Snap = a.value("snap", false);
                    for (auto& b : a.value("bindings", nlohmann::json::array()))
                    {
                        std::string kind = b.value("kind", std::string("key"));
                        InputBinding::Kind k = kind == "mouse" ? InputBinding::Kind::MouseButton
                                             : kind == "button" ? InputBinding::Kind::GamepadButton
                                             : kind == "axis"   ? InputBinding::Kind::GamepadAxis
                                                                : InputBinding::Kind::Key;
                        action.Bindings.push_back({ k, b.value("code", 0), b.value("scale", 1.0f) });
                    }
                    s_Config.InputActions.push_back(std::move(action));
                }
                s_Config.ImportRules.clear();
                for (auto& r : j.value("importRules", nlohmann::json::array()))
                    s_Config.ImportRules.push_back({ r.value("folder", std::string()), (AssetType)r.value("type", 0),
                                                     r.value("settings", nlohmann::json::object()) });
                s_Config.DefaultPresets.clear();
                nlohmann::json presets = j.value("defaultPresets", nlohmann::json::object()); // (items() of a temporary would dangle)
                for (auto& [component, preset] : presets.items())
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
        if (packed)
        {
            // A shipped game: its assets come in one archive, unpacked once per version into the user's app data.
            fs::path mounted = AssetPack::Mount(dir / "Assets.pak", s_Config.Name);
            if (mounted.empty()) return false;
            s_AssetsDirectory = mounted;
        }
        AssetManager::Init(s_AssetsDirectory);
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
        nlohmann::json navAgentTypes = nlohmann::json::array();
        for (const auto& t : s_Config.NavAgentTypes)
            navAgentTypes.push_back({ { "name", t.Name }, { "radius", t.Radius }, { "height", t.Height }, { "stepHeight", t.StepHeight }, { "maxSlope", t.MaxSlope } });
        nlohmann::json importRules = nlohmann::json::array();
        for (const ImportRule& r : s_Config.ImportRules)
            importRules.push_back({ { "folder", r.Folder }, { "type", (int)r.Type }, { "settings", r.Settings } });
        nlohmann::json inputActions = nlohmann::json::array();
        for (const InputAction& a : s_Config.InputActions)
        {
            nlohmann::json bindings = nlohmann::json::array();
            for (const InputBinding& b : a.Bindings)
            {
                const char* kind = b.Type == InputBinding::Kind::MouseButton ? "mouse"
                                 : b.Type == InputBinding::Kind::GamepadButton ? "button"
                                 : b.Type == InputBinding::Kind::GamepadAxis ? "axis"
                                                                              : "key";
                bindings.push_back({ { "kind", kind }, { "code", b.Code }, { "control", InputActions::ControlName(b.Type, b.Code) }, { "scale", b.Scale } });
            }
            nlohmann::json action{ { "name", a.Name }, { "axis", a.Axis }, { "deadZone", a.DeadZone }, { "bindings", bindings } };
            if (a.Sensitivity > 0.0f) action["sensitivity"] = a.Sensitivity, action["gravity"] = a.Gravity, action["snap"] = a.Snap;
            inputActions.push_back(action);
        }
        nlohmann::json defaultPresets = nlohmann::json::object();
        for (auto& [component, preset] : s_Config.DefaultPresets) defaultPresets[component] = (uint64_t)preset;
        nlohmann::json j{ { "name", s_Config.Name },
                          { "version", s_Config.Version },
                          { "companyName", s_Config.CompanyName },
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
                          { "packAssets", s_Config.PackAssets },
                          { "bundleDotNet", s_Config.BundleDotNet },
                          { "aheadOfTimeScripts", s_Config.AheadOfTimeScripts },
                          { "trimDotNet", s_Config.TrimDotNet },
                          { "physicsLayers", s_Config.PhysicsLayers },
                          { "layerCollisionMatrix", s_Config.LayerCollisionMatrix },
                          { "navAreas", navAreas },
                          { "navPathBudget", s_Config.NavPathBudget },
                          { "navAgentTypes", navAgentTypes },
                          { "defaultPresets", defaultPresets },
                          { "importRules", importRules },
                          { "defaultLanguage", s_Config.DefaultLanguage },
                          { "engineVersion", "0.1.0" } };
        if (s_Config.InputActionsSet) j["inputActions"] = inputActions;
        // Quality: written once the project changes the defaults (projects that never do keep their file as it was).
        if (!s_Config.QualityLevels.empty() || s_Config.DefaultQuality != 3)
        {
            j["defaultQuality"] = s_Config.DefaultQuality;
            nlohmann::json levels = nlohmann::json::array();
            for (const QualityLevel& q : s_Config.QualityLevels)
                levels.push_back({ { "name", q.Name }, { "maxMsaa", q.MaxMSAA }, { "ssao", q.AmbientOcclusion }, { "ssr", q.ScreenSpaceReflections },
                                   { "ssgi", q.ScreenSpaceGI }, { "bloom", q.Bloom }, { "depthOfField", q.DepthOfField }, { "motionBlur", q.MotionBlur },
                                   { "cloudShadows", q.CloudShadows }, { "maxRaySteps", q.MaxRaySteps }, { "maxGiRays", q.MaxGIRays },
                                   { "shadowResolution", q.ShadowResolution }, { "shadowDistance", q.ShadowDistance }, { "renderScale", q.RenderScale } });
            if (!levels.empty()) j["quality"] = levels;
        }
        std::ofstream out(s_Directory / kFileName);
        out << j.dump(2);
        return (bool)out;
    }

    bool Project::IsOpen() { return s_Open; }
    ProjectConfig& Project::GetConfig() { return s_Config; }
    const fs::path& Project::GetDirectory() { return s_Directory; }
    fs::path Project::GetAssetsDirectory() { return s_AssetsDirectory.empty() ? s_Directory / "Assets" : s_AssetsDirectory; }
}
