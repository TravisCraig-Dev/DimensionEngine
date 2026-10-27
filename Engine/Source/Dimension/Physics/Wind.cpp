#include "Dimension/Physics/Wind.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/Components.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace Dimension::Wind
{
    Field Gather(Scene& scene, float time)
    {
        Field field;
        field.Time = time;
        for (auto [e, zone, t] : scene.GetRegistry().ViewOf<WindZoneComponent, TransformComponent>())
        {
            if (!t.ActiveInHierarchy || zone.Main == 0.0f) continue;
            glm::vec3 forward = -glm::vec3(t.World[2]);
            float len = glm::length(forward);
            field.Zones.push_back({ t.GetWorldPosition(), len > 1e-6f ? forward / len : glm::vec3(0.0f, 0.0f, -1.0f), zone.Mode == WindZoneMode::Spherical,
                                    std::max(zone.Radius, 0.01f), zone.Main, std::max(zone.Turbulence, 0.0f), zone.PulseMagnitude,
                                    std::max(zone.PulseFrequency, 0.0f) });
        }
        return field;
    }

    glm::vec3 Field::At(const glm::vec3& p) const
    {
        glm::vec3 total(0.0f);
        for (const Zone& z : Zones)
        {
            glm::vec3 dir = z.Direction;
            float strength = 1.0f;
            if (z.Spherical)
            {
                glm::vec3 d = p - z.Position;
                float dist = glm::length(d);
                if (dist >= z.Radius) continue;
                dir = dist > 1e-4f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);
                strength = 1.0f - dist / z.Radius; // fading out to its edge
            }
            // Gusts: the whole zone strengthens and eases off together.
            float pulse = std::max(1.0f + z.PulseMagnitude * std::sin(glm::two_pi<float>() * z.PulseFrequency * Time), 0.0f);
            glm::vec3 wind = dir * (z.Main * strength * pulse);
            // Turbulence: swirls drifting through the zone, a fraction of Main.
            if (z.Turbulence > 0.0f)
            {
                float t = Time;
                glm::vec3 swirl(std::sin(p.z * 0.31f + t * 1.7f + std::sin(p.x * 0.23f + t * 0.9f)),
                                0.5f * std::sin(p.x * 0.17f + p.z * 0.11f + t * 2.3f),
                                std::sin(p.x * 0.27f - t * 1.3f + std::sin(p.z * 0.19f - t * 0.7f)));
                wind += swirl * (std::abs(z.Main) * z.Turbulence * strength);
            }
            total += wind;
        }
        return total;
    }

    glm::vec3 DragForce(const glm::vec3& wind, const glm::vec3& velocity, float area)
    {
        constexpr float kAirDensity = 1.2f; // kg/m^3, with a drag coefficient of 1
        glm::vec3 relative = wind - velocity;
        return 0.5f * kAirDensity * area * glm::length(relative) * relative;
    }
}
