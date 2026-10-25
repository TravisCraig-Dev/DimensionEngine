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

    enum class AnimMotionType : int32_t { Clip = 0, BlendTree1D = 1, BlendTree2D = 2 };

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
        glm::vec2 EditorPosition{ 0.0f }; // graph layout
    };

    struct AnimTransition
    {
        int From = -1; // -1 = Any State
        int To = 0;
        bool HasExitTime = false;
        float ExitTime = 1.0f;  // normalized time of the source state (1 = end of the clip)
        float Duration = 0.25f; // crossfade seconds
        std::vector<AnimCondition> Conditions;
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
        int DefaultState = 0;
        glm::vec2 AnyStatePosition{ -220.0f, 0.0f };
        std::vector<AnimLayer> Layers;

        // Set on a layer's machine: parameters are looked up in the controller that owns the layer.
        mutable const AnimatorController* ParameterOwner = nullptr;

        int FindState(const std::string& name) const;
        const AnimParameter* FindParameter(const std::string& name) const;
        // Points every layer's machine at this controller's parameters (call after copying or editing layers).
        void LinkLayers() const;
        // Fixes indices after a state is removed.
        void RemoveState(int index);

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
    };

    // Per-entity state machine state (runtime only).
    struct AnimatorRuntime
    {
        int State = -1;
        float StateTime = 0.0f; // seconds in the current state (scaled by speed, not wrapped)
        std::unordered_map<std::string, float> Parameters;
        std::unordered_set<std::string> Triggers;
        std::vector<std::string> FiredEvents; // animation events passed during the last Update (cleared by Update)
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
        // Weights of a blend tree state from the runtime's parameter values (1D or 2D).
        void TreeWeights(const AnimatorController& controller, const AnimatorRuntime& runtime, const AnimState& state, std::vector<float>& weights);
        // Appends the functions of events whose time lies in (from, to] (normalized; loops repeat every 1.0).
        void CollectEvents(const AnimState& state, float from, float to, std::vector<std::string>& out);
    }
}
