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
        if (ParameterOwner && ParameterOwner != this) return ParameterOwner->FindParameter(name);
        for (auto& p : Parameters)
            if (p.Name == name) return &p;
        return nullptr;
    }

    void AnimatorController::LinkLayers() const
    {
        for (auto& layer : Layers)
        {
            layer.Machine.ParameterOwner = this;
            layer.Machine.LinkLayers();
        }
    }

    const AnimatorController* AnimatorController::LayerMachine(int layer) const
    {
        if (layer == 0) return this;
        return layer > 0 && layer <= (int)Layers.size() ? &Layers[layer - 1].Machine : nullptr;
    }

    void AnimatorController::SyncLayerStates()
    {
        for (size_t li = 0; li < Layers.size(); ++li)
        {
            AnimLayer& layer = Layers[li];
            // Only earlier, unsynced layers can be followed.
            if (layer.SyncedLayer < 0) continue;
            if (layer.SyncedLayer > (int)li || (layer.SyncedLayer > 0 && Layers[layer.SyncedLayer - 1].SyncedLayer >= 0))
            {
                layer.SyncedLayer = -1;
                continue;
            }
            const AnimatorController& src = *LayerMachine(layer.SyncedLayer);
            auto& states = layer.Machine.States;
            states.resize(src.States.size());
            for (size_t i = 0; i < states.size(); ++i)
            {
                states[i].Name = src.States[i].Name;
                states[i].EditorPosition = src.States[i].EditorPosition;
                states[i].Speed = src.States[i].Speed;
                states[i].Loop = src.States[i].Loop;
                states[i].Events.clear(); // the source's events fire; a synced copy would fire them twice
            }
            layer.Machine.Transitions = src.Transitions;
            layer.Machine.Groups = src.Groups;
            for (size_t i = 0; i < states.size(); ++i) states[i].Group = src.States[i].Group;
            layer.Machine.DefaultState = src.DefaultState;
            layer.Machine.AnyStatePosition = src.AnyStatePosition;
            layer.Machine.EntryPosition = src.EntryPosition;
            layer.Machine.ExitPosition = src.ExitPosition;
        }
    }

    void AnimatorController::RemoveState(int index)
    {
        if (index < 0 || index >= (int)States.size()) return;
        States.erase(States.begin() + index);
        std::erase_if(Transitions, [&](const AnimTransition& t) { return t.From == index || (t.To == index && t.ToGroup < 0 && !t.ToExit); });
        for (auto& t : Transitions)
        {
            if (t.From > index) --t.From;
            if (t.To > index) --t.To;
        }
        if (DefaultState >= (int)States.size()) DefaultState = 0;
        else if (DefaultState > index) --DefaultState;
        for (auto& g : Groups)
        {
            if (g.DefaultState == index) g.DefaultState = -1;
            else if (g.DefaultState > index) --g.DefaultState;
        }
    }

    bool AnimatorController::IsInGroup(int state, int group) const
    {
        if (state < 0 || state >= (int)States.size() || group < 0) return false;
        for (int g = States[state].Group, depth = 0; g >= 0 && g < (int)Groups.size() && depth < 64; g = Groups[g].Parent, ++depth)
            if (g == group) return true;
        return false;
    }

    int AnimatorController::EntryState(int group) const
    {
        if (group < 0 || group >= (int)Groups.size()) return -1;
        int d = Groups[group].DefaultState;
        if (d >= 0 && d < (int)States.size() && IsInGroup(d, group)) return d;
        for (int i = 0; i < (int)States.size(); ++i)
            if (IsInGroup(i, group)) return i; // no (valid) default: the first state inside
        return -1;
    }

    void AnimatorController::RemoveGroup(int group)
    {
        if (group < 0 || group >= (int)Groups.size()) return;
        int parent = Groups[group].Parent;
        for (auto& s : States)
            if (s.Group == group) s.Group = parent;
        for (auto& g : Groups)
            if (g.Parent == group) g.Parent = parent;
        std::erase_if(Transitions, [&](const AnimTransition& t) {
            return AnimTransition::GroupOf(t.From) == group || t.ToGroup == group ||
                   (AnimTransition::IsEntry(t.From) && AnimTransition::EntryGroupOf(t.From) == group);
        });
        Groups.erase(Groups.begin() + group);
        auto fix = [&](int& g) { if (g > group) --g; };
        for (auto& s : States) fix(s.Group);
        for (auto& g : Groups) fix(g.Parent);
        for (auto& t : Transitions)
        {
            if (int g = AnimTransition::GroupOf(t.From); g > group) t.From = AnimTransition::FromGroup(g - 1);
            if (AnimTransition::IsEntry(t.From))
                if (int g = AnimTransition::EntryGroupOf(t.From); g > group) t.From = AnimTransition::FromEntry(g - 1);
            fix(t.ToGroup);
        }
    }

    namespace
    {
        // States, transitions and default state of one state machine (the base layer or an extra layer).
        void ReadMachine(const json& j, AnimatorController& c)
        {
            for (auto& s : j.value("states", json::array()))
            {
                AnimState st;
                st.Name = s.value("name", st.Name);
                st.Clip = s.value("clip", std::string());
                st.Speed = s.value("speed", 1.0f);
                st.Loop = s.value("loop", true);
                st.Motion = (AnimMotionType)s.value("motion", 0);
                st.BlendParameter = s.value("blendParameter", std::string());
                st.BlendParameterY = s.value("blendParameterY", std::string());
                for (auto& ch : s.value("children", json::array()))
                    st.Children.push_back({ ch.value("clip", std::string()), ch.value("threshold", 0.0f), ch.value("y", 0.0f) });
                for (auto& ev : s.value("events", json::array()))
                    st.Events.push_back({ ev.value("time", 0.0f), ev.value("function", std::string()) });
                if (s.contains("position")) st.EditorPosition = { s["position"][0].get<float>(), s["position"][1].get<float>() };
                st.Group = s.value("group", -1);
                c.States.push_back(st);
            }
            for (auto& g : j.value("groups", json::array()))
            {
                AnimStateGroup grp;
                grp.Name = g.value("name", grp.Name);
                grp.Parent = g.value("parent", -1);
                grp.DefaultState = g.value("defaultState", -1);
                if (g.contains("position")) grp.EditorPosition = { g["position"][0].get<float>(), g["position"][1].get<float>() };
                if (g.contains("entryPosition")) grp.EntryPosition = { g["entryPosition"][0].get<float>(), g["entryPosition"][1].get<float>() };
                if (g.contains("exitPosition")) grp.ExitPosition = { g["exitPosition"][0].get<float>(), g["exitPosition"][1].get<float>() };
                c.Groups.push_back(grp);
            }
            for (auto& t : j.value("transitions", json::array()))
            {
                AnimTransition tr;
                tr.From = t.value("from", -1);
                tr.To = t.value("to", 0);
                tr.HasExitTime = t.value("hasExitTime", false);
                tr.ExitTime = t.value("exitTime", 1.0f);
                tr.Duration = t.value("duration", 0.25f);
                tr.ToGroup = t.value("toGroup", -1);
                tr.ToExit = t.value("toExit", false);
                for (auto& cond : t.value("conditions", json::array()))
                    tr.Conditions.push_back({ cond.value("parameter", std::string()), (AnimConditionOp)cond.value("op", 4), cond.value("threshold", 0.0f) });
                c.Transitions.push_back(tr);
            }
            c.DefaultState = j.value("defaultState", 0);
            if (j.contains("anyStatePosition")) c.AnyStatePosition = { j["anyStatePosition"][0].get<float>(), j["anyStatePosition"][1].get<float>() };
            if (j.contains("entryPosition")) c.EntryPosition = { j["entryPosition"][0].get<float>(), j["entryPosition"][1].get<float>() };
            if (j.contains("exitPosition")) c.ExitPosition = { j["exitPosition"][0].get<float>(), j["exitPosition"][1].get<float>() };
        }

        void WriteMachine(const AnimatorController& c, json& j)
        {
            j["states"] = json::array();
            for (auto& s : c.States)
            {
                json st{ { "name", s.Name }, { "clip", s.Clip }, { "speed", s.Speed }, { "loop", s.Loop },
                         { "position", { s.EditorPosition.x, s.EditorPosition.y } } };
                if (s.Motion != AnimMotionType::Clip)
                {
                    st["motion"] = (int)s.Motion;
                    st["blendParameter"] = s.BlendParameter;
                    if (IsBlendTree2D(s.Motion)) st["blendParameterY"] = s.BlendParameterY;
                    st["children"] = json::array();
                    for (auto& ch : s.Children)
                    {
                        json child{ { "clip", ch.Clip }, { "threshold", ch.Threshold } };
                        if (IsBlendTree2D(s.Motion)) child["y"] = ch.ThresholdY;
                        st["children"].push_back(child);
                    }
                }
                if (!s.Events.empty())
                {
                    st["events"] = json::array();
                    for (auto& ev : s.Events) st["events"].push_back({ { "time", ev.Time }, { "function", ev.Function } });
                }
                if (s.Group >= 0) st["group"] = s.Group;
                j["states"].push_back(std::move(st));
            }
            if (!c.Groups.empty())
            {
                j["groups"] = json::array();
                for (auto& g : c.Groups)
                    j["groups"].push_back({ { "name", g.Name }, { "parent", g.Parent }, { "defaultState", g.DefaultState },
                                            { "position", { g.EditorPosition.x, g.EditorPosition.y } },
                                            { "entryPosition", { g.EntryPosition.x, g.EntryPosition.y } },
                                            { "exitPosition", { g.ExitPosition.x, g.ExitPosition.y } } });
            }
            j["transitions"] = json::array();
            for (auto& t : c.Transitions)
            {
                json conds = json::array();
                for (auto& cond : t.Conditions) conds.push_back({ { "parameter", cond.Parameter }, { "op", (int)cond.Op }, { "threshold", cond.Threshold } });
                json tr{ { "from", t.From }, { "to", t.To }, { "hasExitTime", t.HasExitTime }, { "exitTime", t.ExitTime },
                         { "duration", t.Duration }, { "conditions", conds } };
                if (t.ToGroup >= 0) tr["toGroup"] = t.ToGroup;
                if (t.ToExit) tr["toExit"] = true;
                j["transitions"].push_back(std::move(tr));
            }
            j["defaultState"] = c.DefaultState;
            j["anyStatePosition"] = { c.AnyStatePosition.x, c.AnyStatePosition.y };
            j["entryPosition"] = { c.EntryPosition.x, c.EntryPosition.y };
            j["exitPosition"] = { c.ExitPosition.x, c.ExitPosition.y };
        }
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
            ReadMachine(j, *c);
            for (auto& l : j.value("layers", json::array()))
            {
                AnimLayer layer;
                layer.Name = l.value("name", layer.Name);
                layer.Weight = l.value("weight", 1.0f);
                layer.Blending = (AnimLayerBlending)l.value("blending", 0);
                layer.SyncedLayer = l.value("sync", -1);
                for (auto& m : l.value("mask", json::array()))
                    if (m.is_string()) layer.Mask.push_back(m.get<std::string>());
                ReadMachine(l, layer.Machine);
                c->Layers.push_back(std::move(layer));
            }
            c->SyncLayerStates();
            c->LinkLayers();
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
        WriteMachine(*this, j);
        if (!Layers.empty())
        {
            j["layers"] = json::array();
            for (auto& layer : Layers)
            {
                json l{ { "name", layer.Name }, { "weight", layer.Weight }, { "blending", (int)layer.Blending }, { "mask", layer.Mask } };
                if (layer.SyncedLayer >= 0) l["sync"] = layer.SyncedLayer;
                WriteMachine(layer.Machine, l);
                j["layers"].push_back(std::move(l));
            }
        }
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

        void BlendWeights2D(const AnimState& state, const glm::vec2& p, std::vector<float>& weights)
        {
            const auto& ch = state.Children;
            weights.assign(ch.size(), 0.0f);
            if (ch.empty()) return;
            // Gradient bands: each child's influence falls from 1 at its position to 0 at every other child's position,
            // measured along the line between them; the weight is the smallest of those, then all are normalized.
            float sum = 0.0f;
            for (size_t i = 0; i < ch.size(); ++i)
            {
                glm::vec2 pi(ch[i].Threshold, ch[i].ThresholdY);
                float h = 1.0f;
                for (size_t j = 0; j < ch.size() && h > 0.0f; ++j)
                {
                    if (j == i) continue;
                    glm::vec2 d = glm::vec2(ch[j].Threshold, ch[j].ThresholdY) - pi;
                    float len2 = glm::dot(d, d);
                    if (len2 < 1e-10f) continue; // children at the same spot share
                    h = std::min(h, std::clamp(1.0f - glm::dot(p - pi, d) / len2, 0.0f, 1.0f));
                }
                weights[i] = h;
                sum += h;
            }
            if (sum <= 1e-6f)
            {
                // Cannot happen for a finite point, but stay safe: the nearest child.
                size_t best = 0;
                for (size_t i = 1; i < ch.size(); ++i)
                    if (glm::distance(p, { ch[i].Threshold, ch[i].ThresholdY }) < glm::distance(p, { ch[best].Threshold, ch[best].ThresholdY })) best = i;
                weights.assign(ch.size(), 0.0f);
                weights[best] = 1.0f;
                return;
            }
            for (float& w : weights) w /= sum;
        }

        void BlendWeightsDirectional(const AnimState& state, const glm::vec2& p, std::vector<float>& weights)
        {
            const auto& ch = state.Children;
            weights.assign(ch.size(), 0.0f);
            if (ch.empty()) return;
            auto at = [&](size_t i) { return glm::vec2(ch[i].Threshold, ch[i].ThresholdY); };
            int center = -1;
            std::vector<std::pair<float, int>> dirs; // (angle, child) of the children away from the center
            for (size_t i = 0; i < ch.size(); ++i)
            {
                glm::vec2 v = at(i);
                if (glm::dot(v, v) < 1e-8f) { if (center < 0) center = (int)i; }
                else dirs.push_back({ std::atan2(v.y, v.x), (int)i });
            }
            auto finish = [&](int a, float wa, int b, float wb) {
                // wa and wb are the direction children's shares; the center child (if any) takes the rest.
                float sum = wa + wb;
                if (center < 0 || sum > 1.0f)
                {
                    if (sum <= 1e-6f) { wa = 1.0f; wb = 0.0f; sum = 1.0f; }
                    wa /= sum;
                    wb /= sum;
                }
                else
                    weights[center] = 1.0f - sum;
                if (a >= 0) weights[a] += wa;
                if (b >= 0) weights[b] += wb;
            };
            if (glm::dot(p, p) < 1e-8f || dirs.empty())
            {
                if (center >= 0) weights[center] = 1.0f;
                else
                    for (auto& [angle, i] : dirs) weights[i] = 1.0f / (float)dirs.size();
                return;
            }
            std::sort(dirs.begin(), dirs.end());
            float angle = std::atan2(p.y, p.x);
            constexpr float kTwoPi = 6.28318530718f;
            // The pair of neighbouring directions that encloses the point's angle (wrapping around).
            size_t n = dirs.size();
            size_t hi = 0;
            while (hi < n && dirs[hi].first < angle) ++hi;
            size_t lo = (hi + n - 1) % n;
            hi %= n;
            int a = dirs[lo].second, b = dirs[hi].second;
            float gap = dirs[hi].first - dirs[lo].first;
            if (gap <= 0.0f) gap += kTwoPi;
            // A single direction, or an opening of 180 degrees or more: only the nearer side's child applies, by how far
            // the point reaches along it.
            auto alongOne = [&](int i) {
                glm::vec2 d = at(i);
                finish(i, std::max(glm::dot(p, d) / glm::dot(d, d), 0.0f), -1, 0.0f);
            };
            if (n == 1 || gap >= kTwoPi * 0.5f - 1e-4f)
            {
                float toA = std::abs(std::remainder(angle - dirs[lo].first, kTwoPi)), toB = std::abs(std::remainder(dirs[hi].first - angle, kTwoPi));
                alongOne(toA <= toB ? a : b);
                return;
            }
            // p = wa * A + wb * B: the point in the triangle (center, A, B).
            glm::vec2 A = at(a), B = at(b);
            float det = A.x * B.y - A.y * B.x;
            if (std::abs(det) < 1e-8f) { alongOne(a); return; }
            float wa = (p.x * B.y - p.y * B.x) / det, wb = (A.x * p.y - A.y * p.x) / det;
            finish(a, std::max(wa, 0.0f), b, std::max(wb, 0.0f));
        }

        void BlendWeightsAt(const AnimState& state, const glm::vec2& value, std::vector<float>& weights)
        {
            if (state.Motion == AnimMotionType::BlendTree2DDirectional) BlendWeightsDirectional(state, value, weights);
            else BlendWeights2D(state, value, weights);
        }

        void TreeWeights(const AnimatorController& controller, const AnimatorRuntime& runtime, const AnimState& state, std::vector<float>& weights)
        {
            if (IsBlendTree2D(state.Motion))
                BlendWeightsAt(state, { GetParameter(controller, runtime, state.BlendParameter), GetParameter(controller, runtime, state.BlendParameterY) },
                               weights);
            else
                BlendWeights(state, GetParameter(controller, runtime, state.BlendParameter), weights);
        }

        float StateDuration(const AnimatorController& controller, const AnimatorRuntime& runtime, const AnimState& state,
                            const ClipDurationFn& clipDuration)
        {
            if (state.Motion == AnimMotionType::Clip) return clipDuration(state.Clip);
            std::vector<float> w;
            TreeWeights(controller, runtime, state, w);
            float d = 0.0f;
            for (size_t i = 0; i < w.size(); ++i)
                if (w[i] > 0.0f) d += w[i] * clipDuration(state.Children[i].Clip);
            return d;
        }

        float NormalizedTime(const AnimatorController& controller, const AnimatorRuntime& runtime, const ClipDurationFn& clipDuration)
        {
            if (runtime.State < 0 || runtime.State >= (int)controller.States.size()) return 0.0f;
            const AnimState& state = controller.States[runtime.State];
            if (state.Motion != AnimMotionType::Clip) return runtime.StateTime; // trees track normalized time directly
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
            bool starting = runtime.State < 0 || runtime.State >= (int)controller.States.size();

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

            // Where a transition leads: a state, the state an Entry node picks, or (Exit) where leaving a group leads.
            // Triggers the decision used are added to `used`.
            const int stateCount = (int)controller.States.size();
            std::function<int(int, std::vector<std::string>&, int)> enterGroup, exitGroup;
            auto target = [&](const AnimTransition& t, int sourceGroup, std::vector<std::string>& used, int depth) {
                if (t.ToExit) return exitGroup(sourceGroup, used, depth + 1);
                if (t.ToGroup >= 0) return enterGroup(t.ToGroup, used, depth + 1);
                return t.To;
            };
            enterGroup = [&](int group, std::vector<std::string>& used, int depth) -> int {
                if (depth < 32)
                    for (const AnimTransition& t : controller.Transitions)
                    {
                        if (!AnimTransition::IsEntry(t.From) || AnimTransition::EntryGroupOf(t.From) != group || t.ToExit) continue;
                        std::vector<std::string> mine;
                        if (!conditionsMet(t, mine)) continue;
                        int to = target(t, group, mine, depth);
                        if (to < 0 || to >= stateCount) continue;
                        used.insert(used.end(), mine.begin(), mine.end());
                        return to;
                    }
                if (group < 0) return std::clamp(controller.DefaultState, 0, stateCount - 1);
                return controller.EntryState(group);
            };
            exitGroup = [&](int group, std::vector<std::string>& used, int depth) -> int {
                if (group < 0 || group >= (int)controller.Groups.size()) return enterGroup(-1, used, depth);
                if (depth < 32)
                    for (const AnimTransition& t : controller.Transitions)
                    {
                        if (AnimTransition::GroupOf(t.From) != group) continue;
                        std::vector<std::string> mine;
                        if (!conditionsMet(t, mine)) continue;
                        int to = target(t, controller.Groups[group].Parent, mine, depth);
                        if (to < 0 || to >= stateCount) continue;
                        used.insert(used.end(), mine.begin(), mine.end());
                        return to;
                    }
                return enterGroup(controller.Groups[group].Parent, used, depth);
            };
            if (starting)
            {
                // The top level's Entry node picks the first state (its transitions, else the default state).
                std::vector<std::string> used;
                runtime.State = enterGroup(-1, used, 0);
                if (runtime.State < 0 || runtime.State >= stateCount) runtime.State = std::clamp(controller.DefaultState, 0, stateCount - 1);
                for (auto& trig : used) runtime.Triggers.erase(trig);
                runtime.StateTime = 0.0f;
                enter(controller.States[runtime.State], 0.0f);
            }
            // Time in the current state, and the events it passes.
            {
                const AnimState& state = controller.States[runtime.State];
                float before = NormalizedTime(controller, runtime, clipDuration);
                if (state.Motion != AnimMotionType::Clip)
                {
                    // Blend trees advance in normalized time so children stay in sync.
                    float d = StateDuration(controller, runtime, state, clipDuration);
                    runtime.StateTime += d > 0.0f ? dt * state.Speed / d : 0.0f;
                }
                else
                    runtime.StateTime += dt * state.Speed;
                CollectEvents(state, before, NormalizedTime(controller, runtime, clipDuration), runtime.FiredEvents);
            }

            float normalized = NormalizedTime(controller, runtime, clipDuration);
            // Any-state transitions take priority, then those of the sub-state machines the current state is in, then
            // the current state's own, in list order (like Unity).
            for (int pass = 0; pass < 3; ++pass)
                for (size_t i = 0; i < controller.Transitions.size(); ++i)
                {
                    const AnimTransition& t = controller.Transitions[i];
                    int group = AnimTransition::GroupOf(t.From);
                    bool fromAny = t.From == -1, fromGroup = group >= 0;
                    if (pass == 0 && !fromAny) continue;
                    if (pass == 1 && (!fromGroup || !controller.IsInGroup(runtime.State, group))) continue;
                    if (pass == 2 && (fromAny || fromGroup || t.From != runtime.State)) continue;
                    if (t.HasExitTime && normalized < t.ExitTime) continue;
                    if (!t.HasExitTime && t.Conditions.empty()) continue; // would fire immediately forever
                    if (fromAny && t.ToExit) continue; // Any State has no group to leave
                    std::vector<std::string> used;
                    if (!conditionsMet(t, used)) continue;
                    int sourceGroup = fromGroup ? group : controller.States[runtime.State].Group;
                    int to = target(t, sourceGroup, used, 0);
                    if (to < 0 || to >= stateCount) continue;
                    if ((fromAny || fromGroup) && to == runtime.State) continue; // avoid re-entering every frame
                    for (auto& trig : used) runtime.Triggers.erase(trig);
                    runtime.State = to;
                    runtime.StateTime = 0.0f;
                    enter(controller.States[to], t.Duration);
                    return (int)i;
                }
            return -1;
        }
    }
}
