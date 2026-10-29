#include "Dimension/Scene/ViewAlign.h"
#include "Dimension/Scene/Components.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace Dimension::ViewAlign
{
    namespace
    {
        bool Decompose(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale)
        {
            glm::vec3 skew;
            glm::vec4 perspective;
            return glm::decompose(m, scale, rotation, position, skew, perspective);
        }
    }

    void SetWorldPose(Scene& scene, Entity e, const glm::vec3& position, const glm::quat& rotation)
    {
        if (!scene.IsValid(e)) return;
        scene.UpdateTransforms();
        auto& reg = scene.GetRegistry();
        auto& t = reg.Get<TransformComponent>(e);
        glm::vec3 p, worldScale;
        glm::quat r;
        if (!Decompose(t.World, p, r, worldScale)) worldScale = glm::vec3(1.0f);
        glm::mat4 world = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(glm::normalize(rotation)) * glm::scale(glm::mat4(1.0f), worldScale);
        Entity parent = scene.GetParent(e);
        glm::mat4 parentWorld = parent.IsNull() ? glm::mat4(1.0f) : reg.Get<TransformComponent>(parent).World;
        glm::vec3 lp, ls;
        glm::quat lr;
        if (!Decompose(glm::inverse(parentWorld) * world, lp, lr, ls)) return;
        t.Position = lp;
        t.Rotation = glm::eulerAngles(lr);
        t.Scale = ls;
        scene.UpdateTransforms();
    }

    void AlignWithView(Scene& scene, const std::vector<Entity>& objects, const glm::mat4& cameraWorld)
    {
        glm::vec3 position, scale;
        glm::quat rotation;
        if (!Decompose(cameraWorld, position, rotation, scale)) return;
        for (Entity e : objects) SetWorldPose(scene, e, position, rotation);
    }

    void MoveTo(Scene& scene, const std::vector<Entity>& objects, const glm::vec3& point)
    {
        scene.UpdateTransforms();
        auto& reg = scene.GetRegistry();
        std::vector<Entity> valid;
        glm::vec3 center(0.0f);
        for (Entity e : objects)
            if (scene.IsValid(e))
            {
                valid.push_back(e);
                center += reg.Get<TransformComponent>(e).GetWorldPosition();
            }
        if (valid.empty()) return;
        glm::vec3 offset = point - center / (float)valid.size();
        // World poses first: moving a parent would carry a given child along twice.
        std::vector<std::pair<glm::vec3, glm::quat>> poses;
        for (Entity e : valid)
        {
            glm::vec3 p, s;
            glm::quat r;
            Decompose(reg.Get<TransformComponent>(e).World, p, r, s);
            poses.push_back({ p + offset, r });
        }
        for (size_t i = 0; i < valid.size(); ++i)
        {
            bool underAnother = false;
            for (Entity o : valid)
                if (o != valid[i] && scene.IsDescendantOf(valid[i], o)) underAnother = true;
            if (!underAnother) SetWorldPose(scene, valid[i], poses[i].first, poses[i].second);
        }
    }

    glm::mat4 ViewFrom(Scene& scene, Entity e)
    {
        scene.UpdateTransforms();
        const glm::mat4& world = scene.GetRegistry().Get<TransformComponent>(e).World;
        glm::vec3 position(world[3]);
        glm::vec3 forward = -glm::normalize(glm::vec3(world[2]));
        glm::vec3 up = glm::normalize(glm::vec3(world[1]));
        if (std::abs(glm::dot(forward, up)) > 0.999f) up = glm::vec3(0.0f, 0.0f, -1.0f);
        return glm::lookAt(position, position + forward, up);
    }
}
