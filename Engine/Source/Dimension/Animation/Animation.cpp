#include "Dimension/Animation/Animation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

namespace Dimension
{
    namespace Animation
    {
        void BuildMask(const Skeleton& skeleton, const std::vector<std::string>& joints, std::vector<float>& weights)
        {
            size_t n = skeleton.Joints.size();
            weights.assign(n, joints.empty() ? 1.0f : 0.0f);
            if (joints.empty()) return;
            for (const auto& name : joints)
                if (int j = skeleton.FindJoint(name); j >= 0) weights[j] = 1.0f;
            // Parents come first in Order: children inherit their parent's membership.
            for (int j : skeleton.Order)
                if (int parent = skeleton.Joints[j].Parent; parent >= 0 && weights[parent] > 0.0f) weights[j] = 1.0f;
        }

        void LayerOverride(Pose& base, const Pose& layer, const std::vector<float>& mask, float weight)
        {
            size_t n = std::min({ base.Rotations.size(), layer.Rotations.size(), mask.size() });
            for (size_t j = 0; j < n; ++j)
            {
                float w = weight * mask[j];
                if (w <= 0.0f) continue;
                base.Translations[j] = glm::mix(base.Translations[j], layer.Translations[j], w);
                base.Rotations[j] = glm::slerp(base.Rotations[j], layer.Rotations[j], w);
                base.Scales[j] = glm::mix(base.Scales[j], layer.Scales[j], w);
            }
        }

        void LayerAdditive(Pose& base, const Pose& layer, const Pose& reference, const std::vector<float>& mask, float weight)
        {
            size_t n = std::min({ base.Rotations.size(), layer.Rotations.size(), reference.Rotations.size(), mask.size() });
            for (size_t j = 0; j < n; ++j)
            {
                float w = weight * mask[j];
                if (w <= 0.0f) continue;
                base.Translations[j] += (layer.Translations[j] - reference.Translations[j]) * w;
                glm::quat delta = glm::inverse(reference.Rotations[j]) * layer.Rotations[j];
                base.Rotations[j] = glm::normalize(base.Rotations[j] * glm::slerp(glm::quat(1, 0, 0, 0), delta, w));
                glm::vec3 ratio = layer.Scales[j] / glm::max(glm::abs(reference.Scales[j]), glm::vec3(1e-6f)) * glm::sign(reference.Scales[j]);
                base.Scales[j] *= glm::mix(glm::vec3(1.0f), ratio, w);
            }
        }
    }

    void Skeleton::ComputeOrder()
    {
        Order.clear();
        std::vector<int> depth(Joints.size(), 0);
        for (size_t i = 0; i < Joints.size(); ++i)
            for (int p = Joints[i].Parent; p >= 0 && depth[i] < (int)Joints.size(); p = Joints[p].Parent) ++depth[i];
        for (size_t i = 0; i < Joints.size(); ++i) Order.push_back((int)i);
        std::stable_sort(Order.begin(), Order.end(), [&](int a, int b) { return depth[a] < depth[b]; });
    }

    int Skeleton::FindJoint(const std::string& name) const
    {
        for (size_t i = 0; i < Joints.size(); ++i)
            if (Joints[i].Name == name) return (int)i;
        return -1;
    }

    void Pose::SetBindPose(const Skeleton& skeleton)
    {
        size_t n = skeleton.Joints.size();
        Translations.resize(n);
        Rotations.resize(n);
        Scales.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            Translations[i] = skeleton.Joints[i].Translation;
            Rotations[i] = skeleton.Joints[i].Rotation;
            Scales[i] = skeleton.Joints[i].Scale;
        }
    }

    glm::vec3 Animation::ExtractRootMotion(const Skeleton& skeleton, int root, Pose& pose, RootMotionState& state, const glm::vec3& start,
                                           const glm::vec3& end, float time, bool loop, const std::string& motionKey)
    {
        if (root < 0 || root >= (int)pose.Translations.size()) return glm::vec3(0.0f);
        glm::vec3 now = pose.Translations[root];
        glm::vec3 delta(0.0f);
        if (state.Primed && state.LastMotion == motionKey)
        {
            delta = now - state.LastRoot;
            if (loop && time < state.LastTime - 1e-5f) delta += end - start; // wrapped: carry on from the cycle's end
        }
        state.Primed = true;
        state.LastRoot = now;
        state.LastTime = time;
        state.LastMotion = motionKey;

        // Into model space (through the non-joint ancestors), keep the horizontal part as motion.
        glm::mat3 toModel = glm::mat3(skeleton.RootTransform);
        glm::vec3 modelDelta = toModel * delta;
        modelDelta.y = 0.0f;
        // Pin the pose: remove the root's horizontal offset from the start frame.
        glm::vec3 offset = toModel * (now - start);
        offset.y = 0.0f;
        pose.Translations[root] = now - glm::inverse(toModel) * offset;
        return modelDelta;
    }

    namespace Animation
    {
        void Sample(const Skeleton& skeleton, const AnimationClip& clip, float time, Pose& out)
        {
            out.SetBindPose(skeleton);
            for (const auto& ch : clip.Channels)
            {
                if (ch.Times.empty() || ch.Joint < 0 || ch.Joint >= (int)skeleton.Joints.size()) continue;
                glm::vec4 value;
                if (time <= ch.Times.front()) value = ch.Values.front();
                else if (time >= ch.Times.back()) value = ch.Values.back();
                else
                {
                    size_t k = (size_t)(std::upper_bound(ch.Times.begin(), ch.Times.end(), time) - ch.Times.begin()) - 1;
                    float t0 = ch.Times[k], t1 = ch.Times[k + 1];
                    float t = ch.Interpolation == AnimationInterpolation::Step ? 0.0f : (time - t0) / std::max(t1 - t0, 1e-6f);
                    const glm::vec4& a = ch.Values[k];
                    const glm::vec4& b = ch.Values[k + 1];
                    if (ch.Path == AnimationPath::Rotation)
                    {
                        glm::quat q = glm::slerp(glm::quat(a.w, a.x, a.y, a.z), glm::quat(b.w, b.x, b.y, b.z), t);
                        value = { q.x, q.y, q.z, q.w };
                    }
                    else
                        value = glm::mix(a, b, t);
                }
                switch (ch.Path)
                {
                case AnimationPath::Translation: out.Translations[ch.Joint] = glm::vec3(value); break;
                case AnimationPath::Rotation: out.Rotations[ch.Joint] = glm::normalize(glm::quat(value.w, value.x, value.y, value.z)); break;
                case AnimationPath::Scale: out.Scales[ch.Joint] = glm::vec3(value); break;
                }
            }
        }

        void Blend(const Pose& a, const Pose& b, float t, Pose& out)
        {
            size_t n = std::min(a.Rotations.size(), b.Rotations.size());
            out.Translations.resize(n);
            out.Rotations.resize(n);
            out.Scales.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                out.Translations[i] = glm::mix(a.Translations[i], b.Translations[i], t);
                out.Rotations[i] = glm::slerp(a.Rotations[i], b.Rotations[i], t);
                out.Scales[i] = glm::mix(a.Scales[i], b.Scales[i], t);
            }
        }

        void ComputeSkinMatrices(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& out)
        {
            size_t n = skeleton.Joints.size();
            std::vector<glm::mat4> world(n);
            for (int i : skeleton.Order)
            {
                glm::mat4 local = glm::translate(glm::mat4(1.0f), pose.Translations[i]) * glm::toMat4(pose.Rotations[i]) *
                                  glm::scale(glm::mat4(1.0f), pose.Scales[i]);
                int p = skeleton.Joints[i].Parent;
                world[i] = (p >= 0 ? world[p] : skeleton.RootTransform) * local;
            }
            out.resize(n);
            for (size_t i = 0; i < n; ++i) out[i] = world[i] * skeleton.Joints[i].InverseBind;
        }

        const AnimationClip* FindClip(const std::vector<AnimationClip>& clips, const std::string& name)
        {
            for (auto& c : clips)
                if (c.Name == name) return &c;
            return nullptr;
        }
    }
}
