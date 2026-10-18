#include "EditorCamera.h"
#include <Dimension/Core/Input.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>

using namespace Dimension;

namespace Editor
{
    static glm::quat Orientation(float yaw, float pitch)
    {
        return glm::angleAxis(yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(pitch, glm::vec3(1, 0, 0));
    }

    glm::vec3 EditorCamera::GetForward() const { return m_2D ? glm::vec3(0, 0, -1) : Orientation(m_Yaw, m_Pitch) * glm::vec3(0, 0, -1); }
    glm::vec3 EditorCamera::GetRight() const { return m_2D ? glm::vec3(1, 0, 0) : Orientation(m_Yaw, m_Pitch) * glm::vec3(1, 0, 0); }
    glm::vec3 EditorCamera::GetUp() const { return m_2D ? glm::vec3(0, 1, 0) : Orientation(m_Yaw, m_Pitch) * glm::vec3(0, 1, 0); }

    glm::vec3 EditorCamera::GetPosition() const
    {
        if (m_2D) return glm::vec3(m_Pivot.x, m_Pivot.y, 100.0f);
        return m_Pivot - GetForward() * m_Distance;
    }

    glm::mat4 EditorCamera::GetView() const
    {
        return glm::lookAt(GetPosition(), GetPosition() + GetForward(), GetUp());
    }

    glm::mat4 EditorCamera::GetProjection(float aspect) const
    {
        if (m_2D)
        {
            float h = m_OrthoSize, w = h * aspect;
            return glm::ortho(-w, w, -h, h, 0.01f, 1000.0f);
        }
        return glm::perspective(glm::radians(FieldOfView), aspect, NearClip, FarClip);
    }

    void EditorCamera::SetFromView(const glm::mat4& view)
    {
        glm::mat4 inv = glm::inverse(view);
        glm::vec3 position(inv[3]);
        glm::vec3 f = -glm::normalize(glm::vec3(inv[2]));
        m_Pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
        m_Pitch = std::clamp(m_Pitch, -1.5599f, 1.5599f);
        m_Yaw = std::atan2(-f.x, -f.z);
        m_Pivot = position + GetForward() * m_Distance;
    }

    void EditorCamera::Set2D(bool enabled)
    {
        if (m_2D == enabled) return;
        m_2D = enabled;
        if (enabled) m_OrthoSize = std::max(1.0f, m_Distance * 0.5f);
    }

    void EditorCamera::Focus(const glm::vec3& target, float radius)
    {
        m_Pivot = target;
        radius = std::max(radius, 0.25f);
        if (m_2D) m_OrthoSize = radius * 1.5f;
        else m_Distance = radius * 2.5f / std::tan(glm::radians(FieldOfView) * 0.5f) * 0.5f;
    }

    void EditorCamera::Update(float dt, bool hovered)
    {
        bool rmb = Input::RawMouseDown(MouseButton::Right);
        bool mmb = Input::RawMouseDown(MouseButton::Middle);
        bool lmb = Input::RawMouseDown(MouseButton::Left);
        bool alt = Input::RawKeyDown(Key::LeftAlt) || Input::RawKeyDown(Key::RightAlt);
        glm::vec2 delta = Input::GetMouseDelta();
        float scroll = Input::GetScrollDelta();

        // Start controlling only when the gesture begins over the viewport, then keep it until release.
        bool anyButton = rmb || mmb || (alt && lmb);
        if (!m_Controlling && anyButton && hovered && (Input::GetMouseDelta() != glm::vec2(0) || rmb)) m_Controlling = true;
        if (!anyButton) m_Controlling = false;

        if (m_2D)
        {
            if (m_Controlling && (rmb || mmb))
            {
                float unitsPerPixel = m_OrthoSize * 2.0f / 600.0f;
                m_Pivot.x -= delta.x * unitsPerPixel;
                m_Pivot.y += delta.y * unitsPerPixel;
            }
            if (hovered && scroll != 0.0f) m_OrthoSize = std::clamp(m_OrthoSize * (1.0f - scroll * 0.12f), 0.05f, 5000.0f);
            return;
        }

        if (m_Controlling && rmb)
        {
            // Free look around the camera position.
            glm::vec3 position = GetPosition();
            m_Yaw -= delta.x * 0.004f;
            m_Pitch = std::clamp(m_Pitch - delta.y * 0.004f, -1.55f, 1.55f);
            m_Pivot = position + GetForward() * m_Distance;

            glm::vec3 move(0.0f);
            if (Input::RawKeyDown(Key::W)) move += GetForward();
            if (Input::RawKeyDown(Key::S)) move -= GetForward();
            if (Input::RawKeyDown(Key::D)) move += GetRight();
            if (Input::RawKeyDown(Key::A)) move -= GetRight();
            if (Input::RawKeyDown(Key::E)) move += glm::vec3(0, 1, 0);
            if (Input::RawKeyDown(Key::Q)) move -= glm::vec3(0, 1, 0);
            float speed = m_FlySpeed * (Input::RawKeyDown(Key::LeftShift) ? 3.0f : 1.0f);
            m_Pivot += move * speed * dt;
            if (scroll != 0.0f) m_FlySpeed = std::clamp(m_FlySpeed * (1.0f + scroll * 0.15f), 0.1f, 500.0f);
            return;
        }
        if (m_Controlling && alt && lmb)
        {
            m_Yaw -= delta.x * 0.005f;
            m_Pitch = std::clamp(m_Pitch - delta.y * 0.005f, -1.55f, 1.55f);
        }
        if (m_Controlling && mmb)
        {
            float k = m_Distance * 0.0016f;
            m_Pivot += (-GetRight() * delta.x + GetUp() * delta.y) * k;
        }
        if (hovered && scroll != 0.0f && !rmb) m_Distance = std::clamp(m_Distance * (1.0f - scroll * 0.12f), 0.1f, 5000.0f);
    }
}
