#pragma once
#include "Dimension/Core/Base.h"
#include <glm/glm.hpp>
#include <unordered_map>
#include <unordered_set>

namespace Dimension
{
    enum class AnimParameterType : int32_t { Float = 0, Int = 1, Bool = 2, Trigger = 3 };
    enum class AnimConditionOp : int32_t { Greater = 0, Less = 1, Equals = 2, NotEquals = 3, IsTrue = 4, IsFalse = 5 };

    struct AnimParameter
    {
        std::string Name;
        AnimParameterType Type = AnimParameterType::Float;
        float Default = 0.0f;
    };

    struct AnimCondition
    {
        std::string Parameter;
        AnimConditionOp Op = AnimConditionOp::IsTrue; // triggers use IsTrue
        float Threshold = 0.0f;
    };

    // BlendTree2D is Unity's "2D Freeform Cartesian"; BlendTree2DDirectional is "2D Simple Directional" (one motion
    // per direction, plus optionally one at the center).
    enum class AnimMotionType : int32_t { Clip = 0, BlendTree1D = 1, BlendTree2D = 2, BlendTree2DDirectional = 3 };
    inline bool IsBlendTree2D(AnimMotionType m) { return m == AnimMotionType::BlendTree2D || m == AnimMotionType::BlendTree2DDirectional; }

    // One motion of a blend tree: placed at `Threshold` on the blend parameter (1D), or at (Threshold, ThresholdY) on
    // the two parameters (2D).
    struct BlendTreeChild
    {
        std::string Clip;
        float Threshold = 0.0f;
        float ThresholdY = 0.0f;
    };

    // Calls OnAnimationEvent(Function) on the entity's scripts when playback passes `Time` (normalized, 0-1).
    struct AnimEvent
    {
        float Time = 0.5f;
        std::string Function = "OnStep";
    };

    struct AnimState
    {
        std::string Name = "State";
        AnimMotionType Motion = AnimMotionType::Clip;
        std::string Clip;                    // Motion == Clip
        std::string BlendParameter;          // blend trees: float parameter (2D: the X axis)
        std::string BlendParameterY;         // Motion == BlendTree2D: float parameter of the Y axis
        std::vector<BlendTreeChild> Children; // blend trees, any order
        float Speed = 1.0f;
        bool Loop = true;
        std::vector<AnimEvent> Events;
        std::string Tag;                     // for scripts (AnimatorStateInfo.IsTag)
        std::vector<std::string> Behaviours; // C# StateMachineBehaviour classes told when it is entered, updated, left
        glm::vec2 EditorPosition{ 0.0f }; // graph layout
        int Group = -1;                   // sub-state machine (index into Groups); -1 = the top level
    };

    // A sub-state machine: a named group of states (and other groups) shown as one node in the graph. A transition
    // to the group goes through its Entry node: the first entry transition (AnimTransition::FromEntry) whose
    // conditions hold picks the state, otherwise its Default State. Transitions from the group
    // (AnimTransition::FromGroup) apply from any state inside it, like an Any State scoped to the group. A transition
    // to Exit leaves the group: the group's own transitions whose conditions hold are taken, otherwise the parent's
    // entry (as in Unity).
    struct AnimStateGroup
    {
        std::string Name = "Sub-State Machine";
        int Parent = -1;       // enclosing group, -1 = the top level
        int DefaultState = -1; // entered by transitions to the group
        glm::vec2 EditorPosition{ 0.0f }; // in the parent's graph
        glm::vec2 EntryPosition{ -420.0f, 0.0f }, ExitPosition{ 420.0f, 0.0f }; // its Entry and Exit nodes, in its own graph
    };

    struct AnimTransition
    {
        // Source: a state index, -1 = Any State, FromGroup(g) for "any state inside group g", or FromEntry(g) for the
        // Entry node of group g (-1 = the top level: chooses the first state).
        static constexpr int kEntryBase = -1000000;
        static constexpr int FromGroup(int group) { return -2 - group; }
        static constexpr int GroupOf(int from) { return from <= -2 && from > kEntryBase ? -2 - from : -1; }
        static constexpr int FromEntry(int group) { return kEntryBase - 1 - group; }
        static constexpr bool IsEntry(int from) { return from <= kEntryBase; }
        static constexpr int EntryGroupOf(int from) { return kEntryBase - 1 - from; }
        int From = -1; // -1 = Any State
        int To = 0;
        bool HasExitTime = false;
        float ExitTime = 1.0f;  // normalized time of the source state (1 = end of the clip)
        float Duration = 0.25f; // crossfade seconds
        std::vector<AnimCondition> Conditions;
        int ToGroup = -1; // >= 0: into this sub-state machine, at its entry state (To is then unused)
        bool ToExit = false; // leave the source's sub-state machine (To and ToGroup unused)
    };

    enum class AnimLayerBlending : int32_t { Override = 0, Additive = 1 };

    struct AnimLayer;

    // State machine asset (".controller"), like Unity's Animator Controller. The fields below form the base layer;
    // Layers are extra state machines evaluated on top of it (each with a weight, blending mode and joint mask).
    struct AnimatorController
    {
        std::vector<AnimParameter> Parameters;
        std::vector<AnimState> States;
        std::vector<AnimTransition> Transitions;
        std::vector<AnimStateGroup> Groups; // sub-state machines
        int DefaultState = 0;
        glm::vec2 AnyStatePosition{ -220.0f, 0.0f };
        glm::vec2 EntryPosition{ -420.0f, 0.0f }, ExitPosition{ 420.0f, 0.0f }; // the top level's Entry and Exit nodes
        std::vector<AnimLayer> Layers;

        // Set on a layer's machine: parameters are looked up in the controller that owns the layer.
        mutable const AnimatorController* ParameterOwner = nullptr;

        int FindState(const std::string& name) const;
        const AnimParameter* FindParameter(const std::string& name) const;
        // Points every layer's machine at this controller's parameters (call after copying or editing layers).
        void LinkLayers() const;
        // Gives every synced layer the states (names, layout, speed, loop) and transitions of its source layer, keeping
        // the layer's own motion per state. Call after loading or editing the source layer.
        void SyncLayerStates();
        // The machine a layer index refers to (0 = this base layer, n = Layers[n - 1]); null when out of range.
        const AnimatorController* LayerMachine(int layer) const;
        // Fixes indices after a state is removed.
        void RemoveState(int index);
        // True when `state` is inside `group` (directly or in a group within it).
        bool IsInGroup(int state, int group) const;
        // Removes a sub-state machine: its states and groups move to its parent; transitions from it are dropped.
        void RemoveGroup(int group);
        // The group's entry state, following nested groups' defaults; -1 when it has no states.
        int EntryState(int group) const;

        static Ref<AnimatorController> Load(const fs::path& path);
        bool Save(const fs::path& path) const;
    };

    // An extra layer: its own states and transitions, sharing the controller's parameters.
    struct AnimLayer
    {
        std::string Name = "Layer";
        float Weight = 1.0f;
        AnimLayerBlending Blending = AnimLayerBlending::Override;
        std::vector<std::string> Mask; // joints affected, with their descendants; empty = the whole skeleton
        AnimatorController Machine;    // States, Transitions and DefaultState (its Parameters are unused)
        // Synced layer (like Unity's Sync): -1 = off; otherwise an earlier layer (0 = base, n = n-th extra layer) whose
        // state machine drives this one. The layer then plays its own motion for each of that layer's states (a state
        // without one plays the source's), at the source's normalized time and through the same crossfades; its own
        // transitions are unused.
        int SyncedLayer = -1;
    };

    // A state's own motion in a synced layer: a clip, or a blend tree with children. Otherwise the source's plays.
    inline bool HasOwnMotion(const AnimState& s) { return s.Motion == AnimMotionType::Clip ? !s.Clip.empty() : !s.Children.empty(); }

    // Per-entity state machine state (runtime only).
    struct AnimatorRuntime
    {
        int State = -1;
        float StateTime = 0.0f; // seconds in the current state (scaled by speed, not wrapped)
        std::unordered_map<std::string, float> Parameters;
        std::unordered_set<std::string> Triggers;
        std::vector<std::string> FiredEvents; // animation events passed during the last Update (cleared by Update)
        // A state a script asked for (Animator.Play / CrossFade), entered at the next Update before transitions are
        // checked: fading over RequestedFade seconds, starting RequestedOffset into it (normalized, or seconds when
        // RequestedFixedTime).
        int RequestedState = -1;
        float RequestedFade = 0.0f, RequestedOffset = 0.0f;
        bool RequestedFixedTime = false;
    };

    namespace AnimatorStateMachine
    {
        using ClipDurationFn = std::function<float(const std::string& clip)>;
        using EnterStateFn = std::function<void(const AnimState& state, float fadeSeconds)>;

        // Advances the machine: enters the default state on first use, evaluates transitions
        // (consuming triggers that fired) and calls `enter` whenever the state changes.
        // Returns the index of the transition taken this update, or -1.
        int Update(const AnimatorController& controller, AnimatorRuntime& runtime, float dt, const ClipDurationFn& clipDuration,
                   const EnterStateFn& enter);

        float GetParameter(const AnimatorController& controller, const AnimatorRuntime& runtime, const std::string& name);
        float NormalizedTime(const AnimatorController& controller, const AnimatorRuntime& runtime, const ClipDurationFn& clipDuration);
        // Seconds per loop of a state: its clip, or the weighted duration of a blend tree's children.
        float StateDuration(const AnimatorController& controller, const AnimatorRuntime& runtime, const AnimState& state,
                            const ClipDurationFn& clipDuration);
        // Weight per child of a 1D blend tree (sums to 1; at most two non-zero) for the parameter value.
        void BlendWeights(const AnimState& state, float value, std::vector<float>& weights);
        // Weight per child of a 2D blend tree (freeform cartesian, gradient band interpolation): sums to 1, exactly 1 at
        // a child's position, and the children nearest the point share the rest. Works for any child layout.
        void BlendWeights2D(const AnimState& state, const glm::vec2& value, std::vector<float>& weights);
        // Weight per child of a 2D simple directional tree: the point's direction picks the two children whose
        // directions enclose it, blended by their angle and distance, and a child at the center (if any) takes the rest
        // toward the middle. Without a center child only the direction matters. Sums to 1.
        void BlendWeightsDirectional(const AnimState& state, const glm::vec2& value, std::vector<float>& weights);
        // Either 2D weighting, by the state's motion type.
        void BlendWeightsAt(const AnimState& state, const glm::vec2& value, std::vector<float>& weights);
        // Weights of a blend tree state from the runtime's parameter values (1D or 2D).
        void TreeWeights(const AnimatorController& controller, const AnimatorRuntime& runtime, const AnimState& state, std::vector<float>& weights);
        // Appends the functions of events whose time lies in (from, to] (normalized; loops repeat every 1.0).
        void CollectEvents(const AnimState& state, float from, float to, std::vector<std::string>& out);
    }
}
