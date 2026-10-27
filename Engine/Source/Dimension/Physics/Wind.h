#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace Dimension
{
    class Scene;

    // Wind Zones (see WindZoneComponent): the wind's velocity anywhere, from all active zones added up.
    namespace Wind
    {
        struct Zone
        {
            glm::vec3 Position, Direction; // Directional: where it blows (unit); Spherical: unused
            bool Spherical;
            float Radius, Main, Turbulence, PulseMagnitude, PulseFrequency;
        };
        // The scene's zones at `time` (seconds), gathered once and sampled many times.
        struct Field
        {
            std::vector<Zone> Zones;
            float Time = 0.0f;
            bool Empty() const { return Zones.empty(); }
            glm::vec3 At(const glm::vec3& p) const;
        };
        Field Gather(Scene& scene, float time);
        // Air drag pushing a body toward the wind: 0.5 * air density * area * |relative| * relative (newtons), with
        // `area` its cross-section facing the wind (m^2).
        glm::vec3 DragForce(const glm::vec3& wind, const glm::vec3& velocity, float area);
    }
}
