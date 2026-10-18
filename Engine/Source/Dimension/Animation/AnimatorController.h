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

    struct AnimState
    {
        std::string Name = "State";
        std::string Clip;
        float Speed = 1.0f;
        bool Loop = true;
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

    // State machine asset (".controller"), like Unity's Animator Controller.
    struct AnimatorController
    {
        std::vector<AnimParameter> Parameters;
        std::vector<AnimState> States;
        std::vector<AnimTransition> Transitions;
        int DefaultState = 0;
        glm::vec2 AnyStatePosition{ -220.0f, 0.0f };

        int FindState(const std::string& name) const;
        const AnimParameter* FindParameter(const std::string& name) const;
        // Fixes indices after a state is removed.
        void RemoveState(int index);

        static Ref<AnimatorController> Load(const fs::path& path);
        bool Save(const fs::path& path) const;
    };

    // Per-entity state machine state (runtime only).
    struct AnimatorRuntime
    {
        int State = -1;
        float StateTime = 0.0f; // seconds in the current state (scaled by speed, not wrapped)
        std::unordered_map<std::string, float> Parameters;
        std::unordered_set<std::string> Triggers;
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
    }
}
