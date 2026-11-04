#pragma once
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    // Where two colliders touch (2D contacts have z = 0).
    struct ContactPoint
    {
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f }; // the other object's surface there, pointing toward the object told
        float Separation = 0.0f;  // negative while they overlap
    };

    enum class CollisionPhase : int { Enter = 0, Stay = 1, Exit = 2 };

    // A collision as one of its two objects sees it (like Unity's Collision): given to OnCollisionEnter / Stay / Exit.
    struct Collision
    {
        Entity Other = Entity::Null();
        std::vector<ContactPoint> Contacts; // none on Exit
        glm::vec3 Normal{ 0.0f };           // the contacts' normal (toward this object)
        // This object's velocity minus the other's as they met (the speed along Normal is how hard they hit).
        glm::vec3 RelativeVelocity{ 0.0f };
        // How hard they hit (N*s): the speed along the normal times the pair's reduced mass, as for an inelastic hit
        // (about 0 while resting).
        float Impulse = 0.0f;

        // The same collision as the other object sees it.
        Collision Flipped(Entity self) const
        {
            Collision c = *this;
            c.Other = self;
            c.Normal = -Normal;
            c.RelativeVelocity = -RelativeVelocity;
            for (auto& p : c.Contacts) p.Normal = -p.Normal;
            return c;
        }
    };
}
