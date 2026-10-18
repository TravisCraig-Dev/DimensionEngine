#include "Dimension/Animation/AnimatorController.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace Dimension
{
    using json = nlohmann::json;

    int AnimatorController::FindState(const std::string& name) const
    {
        for (size_t i = 0; i < States.size(); ++i)
            if (States[i].Name == name) return (int)i;
        return -1;
    }

    const AnimParameter* AnimatorController::FindParameter(const std::string& name) const
    {
        for (auto& p : Parameters)
            if (p.Name == name) return &p;
        return nullptr;
    }

    void AnimatorController::RemoveState(int index)
    {
        if (index < 0 || index >= (int)States.size()) return;
        States.erase(States.begin() + index);
        std::erase_if(Transitions, [&](const AnimTransition& t) { return t.From == index || t.To == index; });
        for (auto& t : Transitions)
        {
            if (t.From > index) --t.From;
            if (t.To > index) --t.To;
        }
        if (DefaultState >= (int)States.size()) DefaultState = 0;
        else if (DefaultState > index) --DefaultState;
    }

    Ref<AnimatorController> AnimatorController::Load(const fs::path& path)
    {
        auto c = MakeRef<AnimatorController>();
        std::ifstream in(path);
        if (!in) return c; // empty controller
        try
        {
            json j = json::parse(in);
            for (auto& p : j.value("parameters", json::array()))
                c->Parameters.push_back({ p.value("name", std::string()), (AnimParameterType)p.value("type", 0), p.value("default", 0.0f) });
            for (auto& s : j.value("states", json::array()))
            {
                AnimState st;
                st.Name = s.value("name", st.Name);
                st.Clip = s.value("clip", std::string());
                st.Speed = s.value("speed", 1.0f);
                st.Loop = s.value("loop", true);
                if (s.contains("position")) st.EditorPosition = { s["position"][0].get<float>(), s["position"][1].get<float>() };
                c->States.push_back(st);
            }
            for (auto& t : j.value("transitions", json::array()))
            {
                AnimTransition tr;
                tr.From = t.value("from", -1);
                tr.To = t.value("to", 0);
                tr.HasExitTime = t.value("hasExitTime", false);
                tr.ExitTime = t.value("exitTime", 1.0f);
                tr.Duration = t.value("duration", 0.25f);
                for (auto& cond : t.value("conditions", json::array()))
                    tr.Conditions.push_back({ cond.value("parameter", std::string()), (AnimConditionOp)cond.value("op", 4), cond.value("threshold", 0.0f) });
                c->Transitions.push_back(tr);
            }
            c->DefaultState = j.value("defaultState", 0);
            if (j.contains("anyStatePosition")) c->AnyStatePosition = { j["anyStatePosition"][0].get<float>(), j["anyStatePosition"][1].get<float>() };
        }
        catch (const std::exception& e)
        {
            Log::Error("Invalid animator controller '{}': {}", path.string(), e.what());
        }
        return c;
    }

    bool AnimatorController::Save(const fs::path& path) const
    {
        json j;
        j["parameters"] = json::array();
        for (auto& p : Parameters) j["parameters"].push_back({ { "name", p.Name }, { "type", (int)p.Type }, { "default", p.Default } });
        j["states"] = json::array();
        for (auto& s : States)
            j["states"].push_back({ { "name", s.Name }, { "clip", s.Clip }, { "speed", s.Speed }, { "loop", s.Loop },
                                    { "position", { s.EditorPosition.x, s.EditorPosition.y } } });
        j["transitions"] = json::array();
        for (auto& t : Transitions)
        {
            json conds = json::array();
            for (auto& c : t.Conditions) conds.push_back({ { "parameter", c.Parameter }, { "op", (int)c.Op }, { "threshold", c.Threshold } });
            j["transitions"].push_back({ { "from", t.From }, { "to", t.To }, { "hasExitTime", t.HasExitTime }, { "exitTime", t.ExitTime },
                                         { "duration", t.Duration }, { "conditions", conds } });
        }
        j["defaultState"] = DefaultState;
        j["anyStatePosition"] = { AnyStatePosition.x, AnyStatePosition.y };
        std::ofstream out(path);
        out << j.dump(2);
        return (bool)out;
    }

    namespace AnimatorStateMachine
    {
        float GetParameter(const AnimatorController& controller, const AnimatorRuntime& runtime, const std::string& name)
        {
            if (auto it = runtime.Parameters.find(name); it != runtime.Parameters.end()) return it->second;
            if (auto* p = controller.FindParameter(name)) return p->Default;
            return 0.0f;
        }

        float NormalizedTime(const AnimatorController& controller, const AnimatorRuntime& runtime, const ClipDurationFn& clipDuration)
        {
            if (runtime.State < 0 || runtime.State >= (int)controller.States.size()) return 0.0f;
            float duration = clipDuration(controller.States[runtime.State].Clip);
            return duration > 0.0f ? runtime.StateTime / duration : 0.0f;
        }

        int Update(const AnimatorController& controller, AnimatorRuntime& runtime, float dt, const ClipDurationFn& clipDuration,
                   const EnterStateFn& enter)
        {
            if (controller.States.empty()) return -1;
            if (runtime.State < 0 || runtime.State >= (int)controller.States.size())
            {
                runtime.State = std::clamp(controller.DefaultState, 0, (int)controller.States.size() - 1);
                runtime.StateTime = 0.0f;
                enter(controller.States[runtime.State], 0.0f);
            }
            runtime.StateTime += dt * controller.States[runtime.State].Speed;

            auto conditionsMet = [&](const AnimTransition& t, std::vector<std::string>& usedTriggers) {
                for (auto& c : t.Conditions)
                {
                    const AnimParameter* p = controller.FindParameter(c.Parameter);
                    if (p && p->Type == AnimParameterType::Trigger)
                    {
                        if (!runtime.Triggers.contains(c.Parameter)) return false;
                        usedTriggers.push_back(c.Parameter);
                        continue;
                    }
                    float v = GetParameter(controller, runtime, c.Parameter);
                    bool ok = false;
                    switch (c.Op)
                    {
                    case AnimConditionOp::Greater: ok = v > c.Threshold; break;
                    case AnimConditionOp::Less: ok = v < c.Threshold; break;
                    case AnimConditionOp::Equals: ok = std::abs(v - c.Threshold) < 1e-4f; break;
                    case AnimConditionOp::NotEquals: ok = std::abs(v - c.Threshold) >= 1e-4f; break;
                    case AnimConditionOp::IsTrue: ok = v != 0.0f; break;
                    case AnimConditionOp::IsFalse: ok = v == 0.0f; break;
                    }
                    if (!ok) return false;
                }
                return true;
            };

            float normalized = NormalizedTime(controller, runtime, clipDuration);
            // Any-state transitions take priority, then the current state's, in list order (like Unity).
            for (int pass = 0; pass < 2; ++pass)
                for (size_t i = 0; i < controller.Transitions.size(); ++i)
                {
                    const AnimTransition& t = controller.Transitions[i];
                    bool fromAny = t.From < 0;
                    if ((pass == 0) != fromAny) continue;
                    if (!fromAny && t.From != runtime.State) continue;
                    if (t.To < 0 || t.To >= (int)controller.States.size()) continue;
                    if (fromAny && t.To == runtime.State) continue; // avoid re-entering every frame
                    if (t.HasExitTime && normalized < t.ExitTime) continue;
                    if (!t.HasExitTime && t.Conditions.empty()) continue; // would fire immediately forever
                    std::vector<std::string> used;
                    if (!conditionsMet(t, used)) continue;
                    for (auto& trig : used) runtime.Triggers.erase(trig);
                    runtime.State = t.To;
                    runtime.StateTime = 0.0f;
                    enter(controller.States[t.To], t.Duration);
                    return (int)i;
                }
            return -1;
        }
    }
}
