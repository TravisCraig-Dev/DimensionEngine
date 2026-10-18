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
                st.Motion = (AnimMotionType)s.value("motion", 0);
                st.BlendParameter = s.value("blendParameter", std::string());
                for (auto& ch : s.value("children", json::array()))
                    st.Children.push_back({ ch.value("clip", std::string()), ch.value("threshold", 0.0f) });
                for (auto& ev : s.value("events", json::array()))
                    st.Events.push_back({ ev.value("time", 0.0f), ev.value("function", std::string()) });
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
        {
            json st{ { "name", s.Name }, { "clip", s.Clip }, { "speed", s.Speed }, { "loop", s.Loop },
                     { "position", { s.EditorPosition.x, s.EditorPosition.y } } };
            if (s.Motion != AnimMotionType::Clip)
            {
                st["motion"] = (int)s.Motion;
                st["blendParameter"] = s.BlendParameter;
                st["children"] = json::array();
                for (auto& ch : s.Children) st["children"].push_back({ { "clip", ch.Clip }, { "threshold", ch.Threshold } });
            }
            if (!s.Events.empty())
            {
                st["events"] = json::array();
                for (auto& ev : s.Events) st["events"].push_back({ { "time", ev.Time }, { "function", ev.Function } });
            }
            j["states"].push_back(std::move(st));
        }
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

        void BlendWeights(const AnimState& state, float value, std::vector<float>& weights)
        {
            const auto& ch = state.Children;
            weights.assign(ch.size(), 0.0f);
            if (ch.empty()) return;
            // Nearest child at or below the value, and nearest above.
            int lo = -1, hi = -1;
            for (int i = 0; i < (int)ch.size(); ++i)
            {
                if (ch[i].Threshold <= value && (lo < 0 || ch[i].Threshold > ch[lo].Threshold)) lo = i;
                if (ch[i].Threshold >= value && (hi < 0 || ch[i].Threshold < ch[hi].Threshold)) hi = i;
            }
            if (lo < 0) { weights[hi] = 1.0f; return; } // below the lowest threshold
            if (hi < 0 || lo == hi) { weights[lo] = 1.0f; return; }
            float span = ch[hi].Threshold - ch[lo].Threshold;
            float t = span > 1e-6f ? (value - ch[lo].Threshold) / span : 0.0f;
            weights[lo] = 1.0f - t;
            weights[hi] = t;
        }

        float StateDuration(const AnimatorController& controller, const AnimatorRuntime& runtime, const AnimState& state,
                            const ClipDurationFn& clipDuration)
        {
            if (state.Motion == AnimMotionType::Clip) return clipDuration(state.Clip);
            std::vector<float> w;
            BlendWeights(state, GetParameter(controller, runtime, state.BlendParameter), w);
            float d = 0.0f;
            for (size_t i = 0; i < w.size(); ++i)
                if (w[i] > 0.0f) d += w[i] * clipDuration(state.Children[i].Clip);
            return d;
        }

        float NormalizedTime(const AnimatorController& controller, const AnimatorRuntime& runtime, const ClipDurationFn& clipDuration)
        {
            if (runtime.State < 0 || runtime.State >= (int)controller.States.size()) return 0.0f;
            const AnimState& state = controller.States[runtime.State];
            if (state.Motion == AnimMotionType::BlendTree1D) return runtime.StateTime; // trees track normalized time directly
            float duration = clipDuration(state.Clip);
            return duration > 0.0f ? runtime.StateTime / duration : 0.0f;
        }

        void CollectEvents(const AnimState& state, float from, float to, std::vector<std::string>& out)
        {
            if (state.Events.empty() || to <= from) return;
            if (!state.Loop)
            {
                from = std::min(from, 1.0f);
                to = std::min(to, 1.0f);
                for (auto& ev : state.Events)
                    if (ev.Time > from && ev.Time <= to) out.push_back(ev.Function);
                return;
            }
            for (float k = std::floor(from); k <= std::floor(to); k += 1.0f)
                for (auto& ev : state.Events)
                {
                    float t = k + std::clamp(ev.Time, 0.0f, 1.0f);
                    if (t > from && t <= to) out.push_back(ev.Function);
                }
        }

        int Update(const AnimatorController& controller, AnimatorRuntime& runtime, float dt, const ClipDurationFn& clipDuration,
                   const EnterStateFn& enter)
        {
            runtime.FiredEvents.clear();
            if (controller.States.empty()) return -1;
            if (runtime.State < 0 || runtime.State >= (int)controller.States.size())
            {
                runtime.State = std::clamp(controller.DefaultState, 0, (int)controller.States.size() - 1);
                runtime.StateTime = 0.0f;
                enter(controller.States[runtime.State], 0.0f);
            }
            {
                const AnimState& state = controller.States[runtime.State];
                float before = NormalizedTime(controller, runtime, clipDuration);
                if (state.Motion == AnimMotionType::BlendTree1D)
                {
                    // Blend trees advance in normalized time so children stay in sync.
                    float d = StateDuration(controller, runtime, state, clipDuration);
                    runtime.StateTime += d > 0.0f ? dt * state.Speed / d : 0.0f;
                }
                else
                    runtime.StateTime += dt * state.Speed;
                CollectEvents(state, before, NormalizedTime(controller, runtime, clipDuration), runtime.FiredEvents);
            }

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
