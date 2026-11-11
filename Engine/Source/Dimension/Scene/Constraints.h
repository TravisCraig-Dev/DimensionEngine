#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Dimension
{
    class Scene;

    // Constraints (like Unity's Position, Rotation, Scale, Parent and Aim Constraints): an object follows the weighted
    // average of its Sources (other objects) after animation, each frame while playing. Weight blends
    // from the object's rest pose (where it was when the constraint first applied) to the constrained one; Affects
    // picks the axes changed; Maintain Offset keeps the offset the object had from its sources then.
    struct ConstraintSource
    {
        Entity Source = Entity::Null();
        float Weight = 1.0f;
    };

    // The fields every constraint has (a macro, so the component registry sees them as the component's own).
#define DIM_CONSTRAINT_COMMON \
    bool Active = true; \
    float Weight = 1.0f; \
    bool MaintainOffset = true; \
    std::vector<ConstraintSource> Sources; \
    /* Runtime: the rest pose (local), captured when it first applies, with the offsets. */ \
    bool Captured = false; \
    glm::vec3 RestPosition{ 0.0f }, RestScale{ 1.0f }; \
    glm::quat RestRotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    struct PositionConstraintComponent
    {
        DIM_CONSTRAINT_COMMON
        glm::vec3 Offset{ 0.0f }; // world space, added to the sources' average
        bool AffectX = true, AffectY = true, AffectZ = true;
    };

    struct RotationConstraintComponent
    {
        DIM_CONSTRAINT_COMMON
        glm::vec3 Offset{ 0.0f }; // Euler radians, after the sources' average
        bool AffectX = true, AffectY = true, AffectZ = true;
    };

    struct ScaleConstraintComponent
    {
        DIM_CONSTRAINT_COMMON
        glm::vec3 Offset{ 1.0f }; // multiplies the sources' average
        bool AffectX = true, AffectY = true, AffectZ = true;
    };

    // Parent: moves and turns as if a child of each source (by its weight), keeping the offset it had from each.
    struct ParentConstraintComponent
    {
        DIM_CONSTRAINT_COMMON
        bool AffectPosition = true, AffectRotation = true;
        std::vector<glm::mat4> Offsets; // runtime: per source, the object's world transform in the source's space
    };

    enum class ConstraintAxis : int32_t { PlusX = 0, MinusX, PlusY, MinusY, PlusZ, MinusZ };
    enum class AimWorldUp : int32_t { SceneUp = 0, ObjectUp, ObjectRotationUp, None };

    // Aim: turns so its Aim Axis points at the sources' average position, its Up Axis toward the world up.
    struct AimConstraintComponent
    {
        DIM_CONSTRAINT_COMMON
        ConstraintAxis AimAxis = ConstraintAxis::PlusZ;
        ConstraintAxis UpAxis = ConstraintAxis::PlusY;
        AimWorldUp WorldUp = AimWorldUp::SceneUp;
        Entity WorldUpObject = Entity::Null(); // ObjectUp: toward it; ObjectRotationUp: its up
        glm::vec3 Offset{ 0.0f };              // Euler radians, after aiming
    };

    namespace Constraints
    {
        // Applies every active constraint of the scene, parents before children (world transforms updated after).
        void Update(Scene& scene);
        // The unit vector of an axis choice.
        glm::vec3 AxisVector(ConstraintAxis axis);
    }
}
