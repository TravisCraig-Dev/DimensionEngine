#include "Dimension/Scene/Constraints.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/Components.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

namespace Dimension::Constraints
{
    glm::vec3 AxisVector(ConstraintAxis axis)
    {
        switch (axis)
        {
        case ConstraintAxis::PlusX: return { 1, 0, 0 };
        case ConstraintAxis::MinusX: return { -1, 0, 0 };
        case ConstraintAxis::PlusY: return { 0, 1, 0 };
        case ConstraintAxis::MinusY: return { 0, -1, 0 };
        case ConstraintAxis::MinusZ: return { 0, 0, -1 };
        default: return { 0, 0, 1 };
        }
    }

    namespace
    {
        struct Decomposed
        {
            glm::vec3 Position{ 0.0f }, Scale{ 1.0f };
            glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        };

        Decomposed Decompose(const glm::mat4& m)
        {
            Decomposed d;
            glm::vec3 skew;
            glm::vec4 perspective;
            glm::decompose(m, d.Scale, d.Rotation, d.Position, skew, perspective);
            d.Rotation = glm::normalize(d.Rotation);
            return d;
        }

        glm::mat4 ParentWorld(Scene& scene, Entity e)
        {
            Entity p = scene.GetParent(e);
            return p.IsNull() ? glm::mat4(1.0f) : scene.GetRegistry().Get<TransformComponent>(p).World;
        }

        // Sources that count: valid, active, with weight; and their total weight.
        std::vector<std::pair<const glm::mat4*, float>> Sources(Scene& scene, const auto& c)
        {
            std::vector<std::pair<const glm::mat4*, float>> out;
            auto& reg = scene.GetRegistry();
            for (const ConstraintSource& s : c.Sources)
                if (!s.Source.IsNull() && scene.IsValid(s.Source) && s.Weight > 0.0f)
                    if (auto* t = reg.TryGet<TransformComponent>(s.Source); t && t->ActiveInHierarchy) out.push_back({ &t->World, s.Weight });
            return out;
        }

        glm::quat AverageRotation(const std::vector<std::pair<glm::quat, float>>& rs)
        {
            glm::quat sum(0, 0, 0, 0), first = rs.empty() ? glm::quat(1, 0, 0, 0) : rs[0].first;
            for (auto [q, w] : rs)
            {
                if (glm::dot(q, first) < 0.0f) q = -q;
                sum.x += q.x * w, sum.y += q.y * w, sum.z += q.z * w, sum.w += q.w * w;
            }
            return glm::length(sum) > 1e-8f ? glm::normalize(sum) : first;
        }

        // The object's new world pose written back as its local transform.
        void WriteWorld(Scene& scene, Entity e, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
        {
            glm::mat4 world = glm::translate(glm::mat4(1.0f), position) * glm::toMat4(rotation) * glm::scale(glm::mat4(1.0f), scale);
            Decomposed local = Decompose(glm::inverse(ParentWorld(scene, e)) * world);
            auto& t = scene.GetRegistry().Get<TransformComponent>(e);
            t.Position = local.Position;
            t.Rotation = EulerAngles(local.Rotation);
            t.Scale = local.Scale;
            t.World = world;
        }

        void Capture(Scene& scene, Entity e, auto& c)
        {
            if (c.Captured) return;
            auto& t = scene.GetRegistry().Get<TransformComponent>(e);
            c.RestPosition = t.Position;
            c.RestRotation = t.GetRotationQuat();
            c.RestScale = t.Scale;
            c.Captured = true;
        }

        // The rest pose in the world (the local rest under the parent now).
        Decomposed RestWorld(Scene& scene, Entity e, const auto& c)
        {
            glm::mat4 local = glm::translate(glm::mat4(1.0f), c.RestPosition) * glm::toMat4(c.RestRotation) * glm::scale(glm::mat4(1.0f), c.RestScale);
            return Decompose(ParentWorld(scene, e) * local);
        }

        glm::vec3 Pick(const glm::vec3& from, const glm::vec3& to, bool x, bool y, bool z) { return { x ? to.x : from.x, y ? to.y : from.y, z ? to.z : from.z }; }

        void ApplyPosition(Scene& scene, Entity e, PositionConstraintComponent& c)
        {
            auto sources = Sources(scene, c);
            if (sources.empty()) return;
            bool first = !c.Captured;
            Capture(scene, e, c);
            glm::vec3 sum(0.0f);
            float total = 0.0f;
            for (auto [m, w] : sources) sum += glm::vec3((*m)[3]) * w, total += w;
            glm::vec3 target = sum / total;
            Decomposed rest = RestWorld(scene, e, c), now = Decompose(scene.GetRegistry().Get<TransformComponent>(e).World);
            if (first && c.MaintainOffset) c.Offset = rest.Position - target;
            glm::vec3 p = glm::mix(rest.Position, target + c.Offset, std::clamp(c.Weight, 0.0f, 1.0f));
            WriteWorld(scene, e, Pick(rest.Position, p, c.AffectX, c.AffectY, c.AffectZ), now.Rotation, now.Scale);
        }

        void ApplyRotation(Scene& scene, Entity e, RotationConstraintComponent& c)
        {
            auto sources = Sources(scene, c);
            if (sources.empty()) return;
            bool first = !c.Captured;
            Capture(scene, e, c);
            std::vector<std::pair<glm::quat, float>> rs;
            for (auto [m, w] : sources) rs.push_back({ Decompose(*m).Rotation, w });
            glm::quat target = AverageRotation(rs);
            Decomposed rest = RestWorld(scene, e, c), now = Decompose(scene.GetRegistry().Get<TransformComponent>(e).World);
            if (first && c.MaintainOffset) c.Offset = EulerAngles(glm::inverse(target) * rest.Rotation);
            glm::quat q = glm::slerp(rest.Rotation, glm::normalize(target * glm::quat(c.Offset)), std::clamp(c.Weight, 0.0f, 1.0f));
            if (!(c.AffectX && c.AffectY && c.AffectZ))
                q = glm::quat(Pick(EulerAngles(rest.Rotation), EulerAngles(q), c.AffectX, c.AffectY, c.AffectZ));
            WriteWorld(scene, e, now.Position, q, now.Scale);
        }

        void ApplyScale(Scene& scene, Entity e, ScaleConstraintComponent& c)
        {
            auto sources = Sources(scene, c);
            if (sources.empty()) return;
            bool first = !c.Captured;
            Capture(scene, e, c);
            glm::vec3 sum(0.0f);
            float total = 0.0f;
            for (auto [m, w] : sources) sum += Decompose(*m).Scale * w, total += w;
            glm::vec3 target = sum / total;
            Decomposed rest = RestWorld(scene, e, c), now = Decompose(scene.GetRegistry().Get<TransformComponent>(e).World);
            if (first && c.MaintainOffset) c.Offset = rest.Scale / glm::max(glm::abs(target), glm::vec3(1e-6f)) * glm::sign(target);
            glm::vec3 s = glm::mix(rest.Scale, target * c.Offset, std::clamp(c.Weight, 0.0f, 1.0f));
            WriteWorld(scene, e, now.Position, now.Rotation, Pick(rest.Scale, s, c.AffectX, c.AffectY, c.AffectZ));
        }

        void ApplyParent(Scene& scene, Entity e, ParentConstraintComponent& c)
        {
            auto sources = Sources(scene, c);
            if (sources.empty()) return;
            bool first = !c.Captured;
            Capture(scene, e, c);
            Decomposed rest = RestWorld(scene, e, c), now = Decompose(scene.GetRegistry().Get<TransformComponent>(e).World);
            glm::mat4 restWorld = glm::translate(glm::mat4(1.0f), rest.Position) * glm::toMat4(rest.Rotation);
            // Offsets from each counted source (captured once, or none: the object sits on the source).
            if (first || c.Offsets.size() != sources.size())
            {
                c.Offsets.clear();
                for (auto [m, w] : sources)
                {
                    Decomposed sd = Decompose(*m);
                    glm::mat4 sourceRT = glm::translate(glm::mat4(1.0f), sd.Position) * glm::toMat4(sd.Rotation);
                    c.Offsets.push_back(c.MaintainOffset ? glm::inverse(sourceRT) * restWorld : glm::mat4(1.0f));
                }
            }
            glm::vec3 position(0.0f);
            std::vector<std::pair<glm::quat, float>> rs;
            float total = 0.0f;
            for (size_t i = 0; i < sources.size(); ++i)
            {
                Decomposed sd = Decompose(*sources[i].first);
                glm::mat4 sourceRT = glm::translate(glm::mat4(1.0f), sd.Position) * glm::toMat4(sd.Rotation);
                Decomposed want = Decompose(sourceRT * c.Offsets[i]);
                position += want.Position * sources[i].second;
                rs.push_back({ want.Rotation, sources[i].second });
                total += sources[i].second;
            }
            float w = std::clamp(c.Weight, 0.0f, 1.0f);
            glm::vec3 p = c.AffectPosition ? glm::mix(rest.Position, position / total, w) : now.Position;
            glm::quat q = c.AffectRotation ? glm::slerp(rest.Rotation, AverageRotation(rs), w) : now.Rotation;
            WriteWorld(scene, e, p, q, now.Scale);
        }

        void ApplyAim(Scene& scene, Entity e, AimConstraintComponent& c)
        {
            auto sources = Sources(scene, c);
            if (sources.empty()) return;
            bool first = !c.Captured;
            Capture(scene, e, c);
            auto& reg = scene.GetRegistry();
            Decomposed rest = RestWorld(scene, e, c), now = Decompose(reg.Get<TransformComponent>(e).World);
            glm::vec3 sum(0.0f);
            float total = 0.0f;
            for (auto [m, w] : sources) sum += glm::vec3((*m)[3]) * w, total += w;
            glm::vec3 dir = sum / total - now.Position;
            if (glm::dot(dir, dir) < 1e-10f) return;
            dir = glm::normalize(dir);
            // The world up the up axis leans toward.
            glm::vec3 up(0.0f, 1.0f, 0.0f);
            const TransformComponent* upObject = !c.WorldUpObject.IsNull() && scene.IsValid(c.WorldUpObject) ? reg.TryGet<TransformComponent>(c.WorldUpObject) : nullptr;
            if (c.WorldUp == AimWorldUp::ObjectUp && upObject) up = glm::vec3(upObject->World[3]) - now.Position;
            else if (c.WorldUp == AimWorldUp::ObjectRotationUp && upObject) up = glm::vec3(upObject->World * glm::vec4(0, 1, 0, 0));
            else if (c.WorldUp == AimWorldUp::None) up = rest.Rotation * AxisVector(c.UpAxis);
            // A frame with the aim axis along dir and the up axis as near `up` as it gets, mapped from the local axes.
            glm::vec3 aimLocal = AxisVector(c.AimAxis), upLocal = AxisVector(c.UpAxis);
            if (std::abs(glm::dot(aimLocal, upLocal)) > 0.99f) upLocal = std::abs(aimLocal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            glm::vec3 upWorld = up - dir * glm::dot(up, dir);
            if (glm::dot(upWorld, upWorld) < 1e-8f) upWorld = std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) - dir * dir.y : glm::vec3(1, 0, 0) - dir * dir.x;
            upWorld = glm::normalize(upWorld);
            glm::vec3 upLocalOrtho = glm::normalize(upLocal - aimLocal * glm::dot(upLocal, aimLocal));
            glm::mat3 localFrame(aimLocal, upLocalOrtho, glm::cross(aimLocal, upLocalOrtho));
            glm::mat3 worldFrame(dir, upWorld, glm::cross(dir, upWorld));
            glm::quat aimed = glm::normalize(glm::quat_cast(worldFrame * glm::transpose(localFrame)));
            if (first && c.MaintainOffset) c.Offset = EulerAngles(glm::inverse(aimed) * rest.Rotation);
            glm::quat q = glm::slerp(rest.Rotation, glm::normalize(aimed * glm::quat(c.Offset)), std::clamp(c.Weight, 0.0f, 1.0f));
            WriteWorld(scene, e, now.Position, q, now.Scale);
        }

        int Depth(Scene& scene, Entity e)
        {
            int d = 0;
            for (Entity p = scene.GetParent(e); !p.IsNull() && d < 1024; p = scene.GetParent(p)) ++d;
            return d;
        }
    }

    void Update(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        std::vector<Entity> constrained;
        auto collect = [&](auto* tag) {
            using T = std::remove_pointer_t<decltype(tag)>;
            for (auto [e, c, t] : reg.ViewOf<T, TransformComponent>())
                if (c.Active && t.ActiveInHierarchy && std::find(constrained.begin(), constrained.end(), e) == constrained.end()) constrained.push_back(e);
        };
        collect((PositionConstraintComponent*)nullptr);
        collect((RotationConstraintComponent*)nullptr);
        collect((ScaleConstraintComponent*)nullptr);
        collect((ParentConstraintComponent*)nullptr);
        collect((AimConstraintComponent*)nullptr);
        if (constrained.empty()) return;
        // Parents first, so a constrained child sees its parent's result.
        std::stable_sort(constrained.begin(), constrained.end(), [&](Entity a, Entity b) { return Depth(scene, a) < Depth(scene, b); });
        for (Entity e : constrained)
        {
            if (auto* c = reg.TryGet<ParentConstraintComponent>(e); c && c->Active) ApplyParent(scene, e, *c);
            if (auto* c = reg.TryGet<PositionConstraintComponent>(e); c && c->Active) ApplyPosition(scene, e, *c);
            if (auto* c = reg.TryGet<RotationConstraintComponent>(e); c && c->Active) ApplyRotation(scene, e, *c);
            if (auto* c = reg.TryGet<AimConstraintComponent>(e); c && c->Active) ApplyAim(scene, e, *c);
            if (auto* c = reg.TryGet<ScaleConstraintComponent>(e); c && c->Active) ApplyScale(scene, e, *c);
            scene.UpdateTransforms(); // (children and later constraints see it)
        }
    }
}
