#pragma once
#include <glm/glm.hpp>

namespace Editor
{
    // Unity-style scene view camera.
    //  3D: RMB drag = look, RMB + WASD/QE = fly (Shift = fast, wheel = speed),
    //      Alt + LMB = orbit, MMB = pan, wheel = dolly, F = frame selection.
    //  2D: orthographic; RMB/MMB = pan, wheel = zoom.
    class EditorCamera
    {
    public:
        void Update(float dt, bool hovered);
        void Focus(const glm::vec3& target, float radius);
        void Set2D(bool enabled);
        bool Is2D() const { return m_2D; }
        bool IsControlling() const { return m_Controlling; }

        glm::mat4 GetView() const;
        glm::mat4 GetProjection(float aspect) const;
        glm::vec3 GetPosition() const;
        glm::vec3 GetForward() const;
        float GetFlySpeed() const { return m_FlySpeed; }
        void SetFlySpeed(float speed) { m_FlySpeed = speed; }
        float GetDistance() const { return m_Distance; }
        // Adopts the orientation and position of a view matrix (orientation gizmo), keeping the orbit distance.
        void SetFromView(const glm::mat4& view);

        float FieldOfView = 60.0f;
        float NearClip = 0.05f, FarClip = 2000.0f;

    private:
        glm::vec3 GetRight() const;
        glm::vec3 GetUp() const;

        glm::vec3 m_Pivot{ 0.0f, 0.5f, 0.0f };
        float m_Distance = 10.0f;
        float m_Yaw = 0.6f, m_Pitch = -0.45f;
        float m_FlySpeed = 6.0f;
        float m_OrthoSize = 6.0f;
        bool m_2D = false;
        bool m_Controlling = false;
    };
}
