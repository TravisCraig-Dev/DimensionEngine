#include "Dimension/Animation/Animation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Dimension
{
    int HumanoidMap::Mapped() const { return (int)std::count_if(Joints.begin(), Joints.end(), [](int j) { return j >= 0; }); }

    bool HumanoidMap::IsHumanoid() const
    {
        for (HumanBone b : { HumanBone::Hips, HumanBone::Spine, HumanBone::Head, HumanBone::LeftUpperArm, HumanBone::LeftLowerArm,
                             HumanBone::LeftHand, HumanBone::RightUpperArm, HumanBone::RightLowerArm, HumanBone::RightHand,
                             HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftFoot, HumanBone::RightUpperLeg,
                             HumanBone::RightLowerLeg, HumanBone::RightFoot })
            if ((*this)[b] < 0) return false;
        return true;
    }

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
            // Blend shapes the layer animates move toward its weights (masks are by joint: they do not apply).
            if (base.Weights.size() < layer.Weights.size()) base.Weights.resize(layer.Weights.size(), std::numeric_limits<float>::quiet_NaN());
            for (size_t i = 0; i < layer.Weights.size() && weight > 0.0f; ++i)
                if (!std::isnan(layer.Weights[i]))
                    base.Weights[i] = std::isnan(base.Weights[i]) ? layer.Weights[i] : glm::mix(base.Weights[i], layer.Weights[i], std::min(weight, 1.0f));
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
            if (base.Weights.size() < layer.Weights.size()) base.Weights.resize(layer.Weights.size(), std::numeric_limits<float>::quiet_NaN());
            for (size_t i = 0; i < layer.Weights.size() && i < reference.Weights.size() && weight > 0.0f; ++i)
                if (!std::isnan(layer.Weights[i]) && !std::isnan(reference.Weights[i]))
                    base.Weights[i] = (std::isnan(base.Weights[i]) ? 0.0f : base.Weights[i]) + (layer.Weights[i] - reference.Weights[i]) * weight;
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

    void AnimationClip::EventsBetween(float from, float to, bool loop, std::vector<std::string>& out) const
    {
        if (Events.empty() || from == to) return;
        auto range = [&](float a, float b, bool includeStart) {
            for (const Event& e : Events)
                if ((includeStart ? e.Time >= a : e.Time > a) && e.Time <= b) out.push_back(e.Function);
        };
        if (to > from) range(from, to, false);
        else if (loop)
        {
            range(from, Duration, false); // to the end, then round from the start
            range(0.0f, to, true);
        }
    }

    void Pose::SetBindPose(const Skeleton& skeleton)
    {
        size_t n = skeleton.Joints.size();
        Translations.resize(n);
        Rotations.resize(n);
        Scales.resize(n);
        Weights.clear();
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

    glm::vec3 Animation::ExtractRootMotion(const Skeleton& skeleton, int root, Pose& pose, RootMotionState& state, const glm::vec3& start,
                                           const glm::vec3& end, const glm::quat& startRotation, const glm::quat& endRotation, float time,
                                           bool loop, const std::string& motionKey, float& yawDelta)
    {
        yawDelta = 0.0f;
        if (root < 0 || root >= (int)pose.Rotations.size()) return glm::vec3(0.0f);
        // The model-space rotation of the non-joint ancestors (scale removed).
        glm::mat3 m = glm::mat3(skeleton.RootTransform);
        for (int c = 0; c < 3; ++c)
            if (float len = glm::length(m[c]); len > 1e-8f) m[c] /= len;
        glm::quat toModel = glm::normalize(glm::quat_cast(m));
        auto heading = [&](const glm::quat& q) {
            glm::vec3 f = (toModel * q) * glm::vec3(0, 0, 1);
            return std::atan2(f.x, f.z);
        };
        auto wrap = [](float a) { return std::remainder(a, glm::two_pi<float>()); };
        float now = heading(pose.Rotations[root]), first = heading(startRotation), last = heading(endRotation);
        if (state.Primed && state.LastMotion == motionKey)
        {
            float d = now - state.LastYaw;
            if (loop && time < state.LastTime - 1e-5f) d += last - first; // wrapped: carry on from the cycle's end
            yawDelta = wrap(d);
        }
        state.LastYaw = now;
        // Pin the heading at the start frame's, then take the travel as usual, in the turned frame.
        float offset = wrap(now - first);
        glm::quat unturn = glm::angleAxis(-offset, glm::vec3(0, 1, 0));
        pose.Rotations[root] = glm::normalize(glm::inverse(toModel) * unturn * toModel * pose.Rotations[root]);
        glm::vec3 travel = ExtractRootMotion(skeleton, root, pose, state, start, end, time, loop, motionKey);
        return unturn * travel;
    }

    namespace Animation
    {
        glm::vec4 SampleChannel(const AnimationChannel& ch, float time)
        {
            if (ch.Values.empty()) return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            if (time <= ch.Times.front()) return ch.Values.front();
            if (time >= ch.Times.back()) return ch.Values.back();
            size_t k = (size_t)(std::upper_bound(ch.Times.begin(), ch.Times.end(), time) - ch.Times.begin()) - 1;
            float t0 = ch.Times[k], t1 = ch.Times[k + 1], td = std::max(t1 - t0, 1e-6f);
            float t = ch.Interpolation == AnimationInterpolation::Step ? 0.0f : (time - t0) / td;
            const glm::vec4& a = ch.Values[k];
            const glm::vec4& b = ch.Values[k + 1];
            if (ch.Interpolation == AnimationInterpolation::CubicSpline && ch.OutTangents.size() == ch.Values.size() &&
                ch.InTangents.size() == ch.Values.size())
            {
                // Hermite: the keys and their tangents (scaled by the gap between the keys).
                float t2 = t * t, t3 = t2 * t;
                glm::vec4 v = (2.0f * t3 - 3.0f * t2 + 1.0f) * a + (t3 - 2.0f * t2 + t) * td * ch.OutTangents[k] + (-2.0f * t3 + 3.0f * t2) * b +
                              (t3 - t2) * td * ch.InTangents[k + 1];
                if (ch.Path == AnimationPath::Rotation)
                {
                    float len = glm::length(v);
                    v = len > 1e-8f ? v / len : b;
                }
                return v;
            }
            if (ch.Path == AnimationPath::Rotation)
            {
                glm::quat q = glm::slerp(glm::quat(a.w, a.x, a.y, a.z), glm::quat(b.w, b.x, b.y, b.z), t);
                return { q.x, q.y, q.z, q.w };
            }
            return glm::mix(a, b, t);
        }

        bool ApplyHumanoidIK(const Skeleton& skeleton, const HumanoidMap& map, Pose& pose, const HumanoidIKGoals& goals)
        {
            if (!map.IsHumanoid() || pose.Rotations.size() != skeleton.Joints.size()) return false;
            static const HumanBone tips[4] = { HumanBone::LeftFoot, HumanBone::RightFoot, HumanBone::LeftHand, HumanBone::RightHand };
            std::vector<glm::mat4> globals;
            for (int g = 0; g < 4; ++g)
            {
                const HumanoidIKGoals::Limb& limb = goals.Limbs[g];
                int tip = map[tips[g]];
                if (tip < 0) continue;
                if (limb.PositionWeight > 0.0f)
                {
                    // The bend toward the hint (by its weight, from where the knee or elbow is now).
                    glm::vec3 pole(0.0f);
                    int middle = skeleton.Joints[tip].Parent;
                    if (limb.HintWeight > 0.0f && middle >= 0)
                    {
                        JointGlobals(skeleton, pose, globals);
                        pole = glm::mix(glm::vec3(globals[middle][3]), limb.Hint, std::min(limb.HintWeight, 1.0f));
                    }
                    SolveIK(skeleton, pose, tip, 2, limb.Position, limb.HintWeight > 0.0f && middle >= 0 ? &pole : nullptr, std::min(limb.PositionWeight, 1.0f));
                }
                if (limb.RotationWeight > 0.0f)
                {
                    // The hand or foot turned to the goal (model space), under its parent as posed now.
                    JointGlobals(skeleton, pose, globals);
                    int parent = skeleton.Joints[tip].Parent;
                    glm::mat3 pm = parent >= 0 ? glm::mat3(globals[parent]) : glm::mat3(skeleton.RootTransform);
                    for (int c = 0; c < 3; ++c) pm[c] = glm::normalize(pm[c]);
                    glm::quat local = glm::normalize(glm::inverse(glm::quat_cast(pm)) * limb.Rotation);
                    pose.Rotations[tip] = glm::slerp(pose.Rotations[tip], local, std::min(limb.RotationWeight, 1.0f));
                }
            }
            int head = map[HumanBone::Head];
            if (goals.LookAtWeight > 0.0f && head >= 0)
            {
                // The head's forward: the model's +Z as the bind pose holds the head.
                Pose bind;
                bind.SetBindPose(skeleton);
                JointGlobals(skeleton, bind, globals);
                glm::mat3 hm(globals[head]);
                for (int c = 0; c < 3; ++c) hm[c] = glm::normalize(hm[c]);
                glm::vec3 aim = glm::normalize(glm::inverse(glm::quat_cast(hm)) * glm::vec3(0.0f, 0.0f, 1.0f));
                SolveLookAt(skeleton, pose, head, aim, 3, goals.LookAt, 90.0f, std::min(goals.LookAtWeight, 1.0f));
            }
            return true;
        }

        bool PlantFeet(const Skeleton& skeleton, const HumanoidMap& map, Pose& pose, const glm::mat4& world, float reach, const GroundQuery& ground)
        {
            int feet[2] = { map[HumanBone::LeftFoot], map[HumanBone::RightFoot] };
            int hips = map[HumanBone::Hips];
            if (!map.IsHumanoid() || feet[0] < 0 || feet[1] < 0 || hips < 0 || pose.Translations.size() != skeleton.Joints.size()) return false;
            std::vector<glm::mat4> globals, bindGlobals;
            Pose bind;
            bind.SetBindPose(skeleton);
            JointGlobals(skeleton, bind, bindGlobals);
            JointGlobals(skeleton, pose, globals);
            glm::mat4 toModel = glm::inverse(world);
            float objectY = world[3].y, scaleY = glm::length(glm::vec3(world[1]));
            // Each foot: its ground, and how far the ankle must move (up or down) to stand on it as it stood on the level.
            float offset[2] = { 0.0f, 0.0f };
            glm::vec3 normal[2] = { glm::vec3(0, 1, 0), glm::vec3(0, 1, 0) };
            bool found[2] = { false, false };
            for (int f = 0; f < 2; ++f)
            {
                glm::vec3 foot = glm::vec3(world * globals[feet[f]][3]);
                float ankle = bindGlobals[feet[f]][3].y * scaleY; // the ankle's height over the level at rest
                float lift = std::max(foot.y - objectY - ankle, 0.0f);
                glm::vec3 point, n;
                if (!ground(glm::vec3(foot.x, objectY + reach, foot.z), reach * 2.0f, point, n)) continue;
                found[f] = true;
                offset[f] = point.y + ankle + lift - foot.y;
                normal[f] = glm::length(n) > 1e-4f ? glm::normalize(n) : glm::vec3(0, 1, 0);
            }
            if (!found[0] && !found[1]) return false;
            // The hips come down as far as the lower foot needs (the higher one bends its knee).
            float drop = std::min({ found[0] ? offset[0] : 0.0f, found[1] ? offset[1] : 0.0f, 0.0f });
            if (drop < 0.0f)
            {
                int parent = skeleton.Joints[hips].Parent;
                glm::vec3 deltaModel = glm::vec3(toModel * glm::vec4(0.0f, drop, 0.0f, 0.0f));
                glm::mat3 pm = parent >= 0 ? glm::mat3(globals[parent]) : glm::mat3(skeleton.RootTransform);
                pose.Translations[hips] += glm::inverse(pm) * deltaModel;
                JointGlobals(skeleton, pose, globals);
            }
            for (int f = 0; f < 2; ++f)
            {
                if (!found[f]) continue;
                // The ankle where it should be (it moved with the hips), then the foot tilted to the slope.
                glm::vec3 foot = glm::vec3(world * globals[feet[f]][3]);
                glm::vec3 target = foot + glm::vec3(0.0f, offset[f] - drop, 0.0f);
                SolveIK(skeleton, pose, feet[f], 2, glm::vec3(toModel * glm::vec4(target, 1.0f)), nullptr, 1.0f);
                glm::vec3 up(0, 1, 0);
                float angle = std::acos(std::clamp(glm::dot(up, normal[f]), -1.0f, 1.0f));
                if (angle > 1e-3f)
                {
                    glm::quat tilt = glm::rotation(up, normal[f]);
                    if (angle > glm::radians(40.0f)) tilt = glm::slerp(glm::quat(1, 0, 0, 0), tilt, glm::radians(40.0f) / angle);
                    JointGlobals(skeleton, pose, globals);
                    auto rotationOf = [](const glm::mat4& m) {
                        glm::mat3 r(m);
                        for (int c = 0; c < 3; ++c) r[c] = glm::normalize(r[c]);
                        return glm::quat_cast(r);
                    };
                    glm::quat worldRot = rotationOf(world), footModel = rotationOf(globals[feet[f]]);
                    glm::quat footWorld = tilt * worldRot * footModel; // turned in the world
                    int parent = skeleton.Joints[feet[f]].Parent;
                    glm::quat parentModel = parent >= 0 ? rotationOf(globals[parent]) : rotationOf(skeleton.RootTransform);
                    pose.Rotations[feet[f]] = glm::normalize(glm::inverse(parentModel) * glm::inverse(worldRot) * footWorld);
                }
            }
            return true;
        }

        int ReduceKeys(AnimationChannel& ch, float rotationDegrees, float distance)
        {
            size_t n = std::min(ch.Times.size(), ch.Values.size());
            if (n < 3 || ch.Interpolation != AnimationInterpolation::Linear) return 0;
            bool rotation = ch.Path == AnimationPath::Rotation;
            float tolerance = rotation ? glm::radians(std::max(rotationDegrees, 0.0f)) : std::max(distance, 0.0f);
            // How far key k is from the line between keys a and b (at its time).
            auto error = [&](size_t a, size_t b, size_t k) {
                float span = ch.Times[b] - ch.Times[a];
                float t = span > 1e-6f ? (ch.Times[k] - ch.Times[a]) / span : 0.0f;
                const glm::vec4 &va = ch.Values[a], &vb = ch.Values[b], &vk = ch.Values[k];
                if (rotation)
                {
                    glm::quat between = glm::slerp(glm::quat(va.w, va.x, va.y, va.z), glm::quat(vb.w, vb.x, vb.y, vb.z), t);
                    float d = std::abs(glm::dot(between, glm::normalize(glm::quat(vk.w, vk.x, vk.y, vk.z))));
                    return 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)); // the angle between them
                }
                glm::vec4 diff = glm::mix(va, vb, t) - vk;
                return glm::length(glm::vec3(diff));
            };
            std::vector<uint8_t> keep(n, 0);
            keep[0] = keep[n - 1] = 1;
            std::vector<std::pair<size_t, size_t>> stack = { { 0, n - 1 } };
            while (!stack.empty())
            {
                auto [a, b] = stack.back();
                stack.pop_back();
                size_t worst = 0;
                float most = -1.0f;
                for (size_t k = a + 1; k < b; ++k)
                    if (float e = error(a, b, k); e > most) most = e, worst = k;
                if (worst == 0 || most <= tolerance) continue;
                keep[worst] = 1;
                stack.push_back({ a, worst });
                stack.push_back({ worst, b });
            }
            int removed = 0;
            size_t w = 0;
            for (size_t k = 0; k < n; ++k)
                if (keep[k])
                {
                    ch.Times[w] = ch.Times[k];
                    ch.Values[w] = ch.Values[k];
                    ++w;
                }
                else
                    ++removed;
            ch.Times.resize(w);
            ch.Values.resize(w);
            return removed;
        }

        int ReduceKeys(AnimationClip& clip, float rotationDegrees, float distance)
        {
            int removed = 0;
            for (AnimationChannel& ch : clip.Channels) removed += ReduceKeys(ch, rotationDegrees, distance);
            for (AnimationChannel& ch : clip.WeightChannels) removed += ReduceKeys(ch, 0.0f, distance);
            return removed;
        }

        void AutoTangents(AnimationChannel& ch)
        {
            size_t n = std::min(ch.Times.size(), ch.Values.size());
            if (ch.Path == AnimationPath::Rotation)
                for (size_t k = 1; k < n; ++k)
                    if (glm::dot(ch.Values[k - 1], ch.Values[k]) < 0.0f) ch.Values[k] = -ch.Values[k];
            ch.Interpolation = AnimationInterpolation::CubicSpline;
            ch.InTangents.assign(n, glm::vec4(0.0f));
            ch.OutTangents.assign(n, glm::vec4(0.0f));
            for (size_t k = 1; k + 1 < n; ++k)
            {
                float span = ch.Times[k + 1] - ch.Times[k - 1];
                glm::vec4 slope = span > 1e-6f ? (ch.Values[k + 1] - ch.Values[k - 1]) / span : glm::vec4(0.0f);
                ch.InTangents[k] = ch.OutTangents[k] = slope;
            }
        }

        void Sample(const Skeleton& skeleton, const AnimationClip& clip, float time, Pose& out)
        {
            out.SetBindPose(skeleton);
            for (const auto& ch : clip.Channels)
            {
                if (ch.Times.empty() || ch.Joint < 0 || ch.Joint >= (int)skeleton.Joints.size()) continue;
                glm::vec4 value = SampleChannel(ch, time);
                switch (ch.Path)
                {
                case AnimationPath::Translation: out.Translations[ch.Joint] = glm::vec3(value); break;
                case AnimationPath::Rotation: out.Rotations[ch.Joint] = glm::normalize(glm::quat(value.w, value.x, value.y, value.z)); break;
                case AnimationPath::Scale: out.Scales[ch.Joint] = glm::vec3(value); break;
                }
            }
            for (const auto& ch : clip.WeightChannels)
            {
                if (ch.Times.empty() || ch.Joint < 0 || ch.Joint > 4096) continue;
                if (out.Weights.size() <= (size_t)ch.Joint) out.Weights.resize(ch.Joint + 1, std::numeric_limits<float>::quiet_NaN());
                out.Weights[ch.Joint] = SampleChannel(ch, time).x;
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
            // Blend shapes: mixed where both animate one, else the one that does.
            out.Weights.assign(std::max(a.Weights.size(), b.Weights.size()), std::numeric_limits<float>::quiet_NaN());
            for (size_t i = 0; i < out.Weights.size(); ++i)
            {
                float wa = i < a.Weights.size() ? a.Weights[i] : std::numeric_limits<float>::quiet_NaN();
                float wb = i < b.Weights.size() ? b.Weights[i] : std::numeric_limits<float>::quiet_NaN();
                out.Weights[i] = std::isnan(wa) ? wb : std::isnan(wb) ? wa : glm::mix(wa, wb, t);
            }
        }

        void JointGlobals(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& out)
        {
            out.resize(skeleton.Joints.size());
            for (int i : skeleton.Order)
            {
                glm::mat4 local = glm::translate(glm::mat4(1.0f), pose.Translations[i]) * glm::toMat4(pose.Rotations[i]) *
                                  glm::scale(glm::mat4(1.0f), pose.Scales[i]);
                int p = skeleton.Joints[i].Parent;
                out[i] = (p >= 0 ? out[p] : skeleton.RootTransform) * local;
            }
        }

        void ComputeSkinMatrices(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& out)
        {
            std::vector<glm::mat4> world;
            JointGlobals(skeleton, pose, world);
            out.resize(world.size());
            for (size_t i = 0; i < world.size(); ++i) out[i] = world[i] * skeleton.Joints[i].InverseBind;
        }

        namespace
        {
            glm::quat GlobalRotation(const glm::mat4& m)
            {
                glm::mat3 r(m);
                for (int c = 0; c < 3; ++c)
                    if (float len = glm::length(r[c]); len > 1e-8f) r[c] /= len;
                return glm::normalize(glm::quat_cast(r));
            }

            // Turns `joint` so that its global rotation becomes `delta * current`.
            void RotateGlobal(const Skeleton& skeleton, Pose& pose, const std::vector<glm::mat4>& globals, int joint, const glm::quat& delta)
            {
                int p = skeleton.Joints[joint].Parent;
                glm::quat parent = GlobalRotation(p >= 0 ? globals[p] : skeleton.RootTransform);
                pose.Rotations[joint] = glm::normalize(glm::inverse(parent) * delta * parent * pose.Rotations[joint]);
            }

            float SafeAcos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }
            glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback)
            {
                float len = glm::length(v);
                return len > 1e-6f ? v / len : fallback;
            }
        }

        bool SolveLookAt(const Skeleton& skeleton, Pose& pose, int joint, const glm::vec3& aimAxis, int chainLength, const glm::vec3& target,
                         float maxAngle, float weight)
        {
            if (weight <= 0.0f || joint < 0 || joint >= (int)skeleton.Joints.size() || pose.Rotations.size() != skeleton.Joints.size()) return false;
            if (glm::dot(aimAxis, aimAxis) < 1e-10f) return false;
            std::vector<int> chain; // the joint and its ancestors, nearest first
            for (int j = joint; j >= 0 && (int)chain.size() < std::max(chainLength, 1); j = skeleton.Joints[j].Parent) chain.push_back(j);
            std::vector<glm::quat> original(pose.Rotations);
            std::vector<glm::mat4> g;
            // Each joint from the top of the chain down takes its share of the turn still needed, re-measured after the
            // joints above it moved (which also moves the tip). The aim stays within `maxAngle` of the animated aim.
            JointGlobals(skeleton, pose, g);
            auto aimOf = [&] { return SafeNormalize(glm::mat3(g[joint]) * aimAxis, { 0, 0, 1 }); };
            const glm::vec3 animated = aimOf();
            const float limit = glm::radians(std::clamp(maxAngle, 0.0f, 180.0f));
            for (int k = (int)chain.size() - 1; k >= 0; --k)
            {
                JointGlobals(skeleton, pose, g);
                glm::vec3 from = aimOf(), to = SafeNormalize(target - glm::vec3(g[joint][3]), from);
                if (float angle = SafeAcos(glm::dot(animated, to)); angle > limit)
                {
                    glm::vec3 axis = glm::cross(animated, to);
                    if (glm::dot(axis, axis) < 1e-12f) return false;
                    to = glm::angleAxis(limit, glm::normalize(axis)) * animated;
                }
                RotateGlobal(skeleton, pose, g, chain[k], glm::slerp(glm::quat(1, 0, 0, 0), glm::rotation(from, to), 1.0f / (float)(k + 1)));
            }
            if (weight < 1.0f)
                for (int j : chain) pose.Rotations[j] = glm::slerp(original[j], pose.Rotations[j], weight);
            return true;
        }

        bool SolveIK(const Skeleton& skeleton, Pose& pose, int tip, int chainLength, const glm::vec3& target, const glm::vec3* hint, float weight)
        {
            if (weight <= 0.0f || tip < 0 || tip >= (int)skeleton.Joints.size() || pose.Rotations.size() != skeleton.Joints.size()) return false;
            std::vector<int> chain; // the tip's ancestors that turn, nearest first
            for (int j = skeleton.Joints[tip].Parent; j >= 0 && (int)chain.size() < std::max(chainLength, 1); j = skeleton.Joints[j].Parent)
                chain.push_back(j);
            if (chain.empty()) return false;
            std::vector<glm::quat> original(pose.Rotations);
            std::vector<glm::mat4> g;
            auto pos = [&](int j) { return glm::vec3(g[j][3]); };
            JointGlobals(skeleton, pose, g);

            if (chain.size() == 2)
            {
                // Analytic two-bone solve (law of cosines): bend the middle joint to the angle the distance needs, then
                // swing the chain's root onto the target. The bend stays in the plane of the hint (or the current bend).
                int a = chain[1], b = chain[0];
                glm::vec3 pa = pos(a), pb = pos(b), pc = pos(tip);
                float lab = glm::length(pb - pa), lcb = glm::length(pc - pb);
                if (lab < 1e-6f || lcb < 1e-6f) return false;
                float lat = std::clamp(glm::length(target - pa), 1e-4f, (lab + lcb) * 0.9999f);
                glm::vec3 ac = SafeNormalize(pc - pa, { 0, 1, 0 });
                if (hint)
                {
                    // Turn the chain about its root-to-tip line so the middle joint lies toward the hint.
                    glm::vec3 cur = pb - pa - ac * glm::dot(pb - pa, ac), want = *hint - pa - ac * glm::dot(*hint - pa, ac);
                    if (glm::dot(cur, cur) > 1e-10f && glm::dot(want, want) > 1e-10f)
                    {
                        RotateGlobal(skeleton, pose, g, a, glm::rotation(glm::normalize(cur), glm::normalize(want)));
                        JointGlobals(skeleton, pose, g);
                        pb = pos(b);
                    }
                }
                glm::vec3 axis = glm::cross(ac, pb - pa);
                if (glm::dot(axis, axis) < 1e-10f && hint) axis = glm::cross(ac, *hint - pa); // straight: bend toward the hint
                if (glm::dot(axis, axis) < 1e-10f) // straight, no hint: bend about any axis perpendicular to the chain
                    axis = glm::cross(ac, std::abs(ac.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1));
                axis = glm::normalize(axis);
                float abAc0 = SafeAcos(glm::dot(ac, glm::normalize(pb - pa)));
                float baBc0 = SafeAcos(glm::dot(glm::normalize(pa - pb), glm::normalize(pc - pb)));
                float abAc1 = SafeAcos((lcb * lcb - lab * lab - lat * lat) / (-2.0f * lab * lat));
                float baBc1 = SafeAcos((lat * lat - lab * lab - lcb * lcb) / (-2.0f * lab * lcb));
                RotateGlobal(skeleton, pose, g, a, glm::angleAxis(abAc1 - abAc0, axis));
                JointGlobals(skeleton, pose, g);
                RotateGlobal(skeleton, pose, g, b, glm::angleAxis(baBc1 - baBc0, axis));
                JointGlobals(skeleton, pose, g);
                glm::vec3 from = SafeNormalize(pos(tip) - pos(a), ac), to = SafeNormalize(target - pos(a), from);
                RotateGlobal(skeleton, pose, g, a, glm::rotation(from, to));
            }
            else
            {
                // One joint or longer chains: cyclic coordinate descent, tip-most joint first.
                for (int iter = 0; iter < 16; ++iter)
                {
                    for (int j : chain)
                    {
                        JointGlobals(skeleton, pose, g);
                        glm::vec3 from = pos(tip) - pos(j), to = target - pos(j);
                        if (glm::dot(from, from) < 1e-10f || glm::dot(to, to) < 1e-10f) continue;
                        RotateGlobal(skeleton, pose, g, j, glm::rotation(glm::normalize(from), glm::normalize(to)));
                    }
                    JointGlobals(skeleton, pose, g);
                    if (glm::distance(pos(tip), target) < 1e-4f) break;
                }
            }
            if (weight < 1.0f)
                for (int j : chain) pose.Rotations[j] = glm::slerp(original[j], pose.Rotations[j], weight);
            return true;
        }

        const AnimationClip* FindClip(const std::vector<AnimationClip>& clips, const std::string& name)
        {
            for (auto& c : clips)
                if (c.Name == name) return &c;
            return nullptr;
        }

        namespace
        {
            std::string JointKey(const std::string& name)
            {
                size_t cut = name.find_last_of(":|");
                std::string key = cut == std::string::npos ? name : name.substr(cut + 1);
                for (char& c : key) c = (char)std::tolower((unsigned char)c);
                return key;
            }
        }

        int MatchJoint(const Skeleton& skeleton, const std::string& name)
        {
            if (int exact = skeleton.FindJoint(name); exact >= 0) return exact;
            std::string key = JointKey(name);
            for (int i = 0; i < (int)skeleton.Joints.size(); ++i)
                if (JointKey(skeleton.Joints[i].Name) == key) return i;
            return -1;
        }

        AnimationClip RetargetClip(const Skeleton& source, const Skeleton& target, const AnimationClip& clip)
        {
            // Root heights (model space, bind pose) set how much root translation scales.
            auto rootHeight = [](const Skeleton& s, int joint) {
                Pose bind;
                bind.SetBindPose(s);
                std::vector<glm::mat4> globals;
                JointGlobals(s, bind, globals);
                return joint >= 0 && joint < (int)globals.size() ? std::abs(globals[joint][3].y) : 0.0f;
            };
            AnimationClip out;
            out.Name = clip.Name;
            out.Duration = clip.Duration;
            out.Events = clip.Events;
            out.PropertyChannels = clip.PropertyChannels; // (by the object's own paths: they carry over)
            for (const AnimationChannel& ch : clip.Channels)
            {
                if (ch.Joint < 0 || ch.Joint >= (int)source.Joints.size()) continue;
                const Joint& from = source.Joints[ch.Joint];
                int t = MatchJoint(target, from.Name);
                if (t < 0) continue;
                const Joint& to = target.Joints[t];
                AnimationChannel mapped = ch;
                mapped.Joint = t;
                if (ch.Path == AnimationPath::Rotation)
                {
                    // The change from the source's bind rotation, applied on the target's bind rotation.
                    glm::quat inverseBind = glm::inverse(from.Rotation);
                    for (glm::vec4& v : mapped.Values)
                    {
                        glm::quat q = to.Rotation * (inverseBind * glm::quat(v.w, v.x, v.y, v.z));
                        v = glm::vec4(q.x, q.y, q.z, q.w);
                    }
                }
                else if (ch.Path == AnimationPath::Translation)
                {
                    // Proportions belong to the target: below the root a joint's translation carries over only where
                    // both skeletons have it in the same place (the same skeleton: hips bobbing, a bowstring drawn),
                    // as its change from that place.
                    float scale = 1.0f;
                    if (to.Parent >= 0 || from.Parent >= 0)
                    {
                        if (glm::distance(from.Translation, to.Translation) > 1e-3f * std::max(1.0f, glm::length(from.Translation))) continue;
                    }
                    else
                    {
                        float hs = rootHeight(source, ch.Joint), ht = rootHeight(target, t);
                        scale = hs > 1e-4f && ht > 1e-4f ? ht / hs : 1.0f;
                    }
                    for (glm::vec4& v : mapped.Values)
                        v = glm::vec4(to.Translation + (glm::vec3(v) - from.Translation) * scale, v.w);
                }
                else
                {
                    // Scale as its ratio to the source's bind scale, on the target's (the same skeleton: exactly the
                    // clip's; a part shown and hidden by scaling it, such as a nocked arrow, carries over).
                    glm::vec3 fromScale = glm::max(glm::abs(from.Scale), glm::vec3(1e-6f));
                    for (glm::vec4& v : mapped.Values)
                        v = glm::vec4(to.Scale * (glm::vec3(v) / fromScale), v.w);
                }
                out.Channels.push_back(std::move(mapped));
            }
            return out;
        }

        // ---- Humanoid avatars

        namespace
        {
            // A joint name as lower-case words (split at separators, case changes and digits), its side (-1 none,
            // 0 left, 1 right) taken out, and the letters left joined into one word.
            struct NameParts
            {
                std::string Core;  // e.g. "upleg" for "mixamorig:LeftUpLeg", "bipthigh" for "Bip01 L Thigh"
                int Side = -1;
            };

            NameParts SplitName(const std::string& name)
            {
                size_t cut = name.find_last_of(":|");
                std::string s = cut == std::string::npos ? name : name.substr(cut + 1);
                std::vector<std::string> words;
                std::string word;
                auto flush = [&]() {
                    if (!word.empty()) words.push_back(word);
                    word.clear();
                };
                for (size_t i = 0; i < s.size(); ++i)
                {
                    char c = s[i];
                    if (!std::isalnum((unsigned char)c))
                    {
                        flush();
                        continue;
                    }
                    bool upper = std::isupper((unsigned char)c) != 0, digit = std::isdigit((unsigned char)c) != 0;
                    // A new word at a lower-to-upper change ("LeftArm") and between letters and digits ("Toe0").
                    if (!word.empty())
                    {
                        char last = s[i - 1];
                        if ((upper && std::islower((unsigned char)last)) || digit != (std::isdigit((unsigned char)last) != 0)) flush();
                    }
                    word += (char)std::tolower((unsigned char)c);
                }
                flush();
                NameParts parts;
                for (const std::string& w : words)
                {
                    if (w == "l" || w == "left" || w == "lf" || w == "lft") parts.Side = 0;
                    else if (w == "r" || w == "right" || w == "rt" || w == "rgt") parts.Side = 1;
                    else if (!std::isdigit((unsigned char)w[0])) parts.Core += w;
                }
                return parts;
            }

            bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

            // Which human bone a joint's name says (spine joints come back as Spine and are ordered later), or Count.
            HumanBone Classify(const NameParts& n)
            {
                const std::string& c = n.Core;
                // Helpers that sit beside or past the real bones (and animation rigs' controls: "HipsCtrl", "LeftKneeCtrl").
                for (const char* skip : { "twist", "roll", "end", "nub", "tip", "ik", "pole", "target", "helper", "dummy", "socket", "prop", "ctrl", "control",
                                          "thumb", "index", "middle", "ring", "pinky", "little", "finger", "eye", "jaw", "tongue", "teeth",
                                          "breast", "weapon", "attach", "corrective", "adjust" })
                    if (Has(c, skip)) return HumanBone::Count;
                auto sided = [&](HumanBone left, HumanBone right) { return n.Side == 0 ? left : n.Side == 1 ? right : HumanBone::Count; };
                if (Has(c, "toe") || Has(c, "ball")) return sided(HumanBone::LeftToes, HumanBone::RightToes);
                if (Has(c, "foot") || Has(c, "ankle")) return sided(HumanBone::LeftFoot, HumanBone::RightFoot);
                if (Has(c, "upleg") || Has(c, "thigh") || Has(c, "upperleg")) return sided(HumanBone::LeftUpperLeg, HumanBone::RightUpperLeg);
                if (Has(c, "calf") || Has(c, "shin") || Has(c, "lowerleg") || Has(c, "knee") || Has(c, "leg"))
                    return sided(HumanBone::LeftLowerLeg, HumanBone::RightLowerLeg);
                if (Has(c, "hand") || Has(c, "wrist")) return sided(HumanBone::LeftHand, HumanBone::RightHand);
                if (Has(c, "forearm") || Has(c, "lowerarm") || Has(c, "elbow")) return sided(HumanBone::LeftLowerArm, HumanBone::RightLowerArm);
                if (Has(c, "clavicle") || Has(c, "shoulder") || Has(c, "collar")) return sided(HumanBone::LeftShoulder, HumanBone::RightShoulder);
                if (Has(c, "arm") && !Has(c, "armature")) return sided(HumanBone::LeftUpperArm, HumanBone::RightUpperArm);
                if (n.Side >= 0) return HumanBone::Count;
                if (Has(c, "hips") || Has(c, "pelvis") || c == "hip") return HumanBone::Hips;
                if (Has(c, "neck")) return HumanBone::Neck;
                if (Has(c, "head") && !Has(c, "top")) return HumanBone::Head;
                if (Has(c, "upperchest")) return HumanBone::UpperChest;
                if (Has(c, "chest") || Has(c, "torso") || Has(c, "spine") || Has(c, "abdomen") || Has(c, "waist")) return HumanBone::Spine;
                return HumanBone::Count;
            }

            glm::quat RotationOf(const glm::mat4& m)
            {
                glm::mat3 r(m);
                for (int c = 0; c < 3; ++c) r[c] /= std::max(glm::length(r[c]), 1e-8f);
                return glm::normalize(glm::quat_cast(r));
            }

            // A humanoid rig's T-pose: each joint's model-space rotation once its human bones are turned to point as a
            // T-posed figure's do (arms straight out to the sides, spine and neck up, legs down, feet forward), in the
            // rig's own frame (its left from the upper arms, up the model's +Y). Rigs bound in an A-pose (or any other)
            // then share a reference: motion is measured from it, not from the pose they were bound in.
            // A humanoid rig's left (from its upper arms, level) in model space; its up is +Y, its forward left x up.
            glm::vec3 BodyLeft(const HumanoidMap& map, const std::vector<glm::mat4>& bind)
            {
                glm::vec3 left = glm::vec3(bind[map[HumanBone::LeftUpperArm]][3]) - glm::vec3(bind[map[HumanBone::RightUpperArm]][3]);
                left.y = 0.0f;
                return glm::length(left) > 1e-6f ? glm::normalize(left) : glm::vec3(1, 0, 0);
            }

            // The human bone a bone hangs from (-1 for the hips).
            int HumanParent(HumanBone bone)
            {
                using B = HumanBone;
                switch (bone)
                {
                case B::Spine: return (int)B::Hips;
                case B::Chest: return (int)B::Spine;
                case B::UpperChest: return (int)B::Chest;
                case B::Neck: return (int)B::UpperChest;
                case B::Head: return (int)B::Neck;
                case B::LeftShoulder: case B::RightShoulder: return (int)B::UpperChest;
                case B::LeftUpperArm: return (int)B::LeftShoulder;
                case B::LeftLowerArm: return (int)B::LeftUpperArm;
                case B::LeftHand: return (int)B::LeftLowerArm;
                case B::RightUpperArm: return (int)B::RightShoulder;
                case B::RightLowerArm: return (int)B::RightUpperArm;
                case B::RightHand: return (int)B::RightLowerArm;
                case B::LeftUpperLeg: case B::RightUpperLeg: return (int)B::Hips;
                case B::LeftLowerLeg: return (int)B::LeftUpperLeg;
                case B::LeftFoot: return (int)B::LeftLowerLeg;
                case B::LeftToes: return (int)B::LeftFoot;
                case B::RightLowerLeg: return (int)B::RightUpperLeg;
                case B::RightFoot: return (int)B::RightLowerLeg;
                case B::RightToes: return (int)B::RightFoot;
                default: return -1;
                }
            }

            std::vector<glm::quat> TPoseRotations(const Skeleton& s, const HumanoidMap& map, const std::vector<glm::mat4>& bind)
            {
                size_t n = s.Joints.size();
                std::vector<glm::quat> delta(n, glm::quat(1, 0, 0, 0)), out(n);
                glm::vec3 left = BodyLeft(map, bind);
                glm::vec3 up(0, 1, 0), forward = glm::normalize(glm::cross(left, up));
                // Each bone, the human bone it points at, and the way that is in a T-pose.
                struct Bone { HumanBone From, To, To2; glm::vec3 Dir; };
                const Bone bones[] = {
                    { HumanBone::Spine, HumanBone::Chest, HumanBone::Neck, up },
                    { HumanBone::Chest, HumanBone::UpperChest, HumanBone::Neck, up },
                    { HumanBone::UpperChest, HumanBone::Neck, HumanBone::Neck, up },
                    { HumanBone::Neck, HumanBone::Head, HumanBone::Head, up },
                    { HumanBone::LeftShoulder, HumanBone::LeftUpperArm, HumanBone::LeftUpperArm, left },
                    { HumanBone::LeftUpperArm, HumanBone::LeftLowerArm, HumanBone::LeftLowerArm, left },
                    { HumanBone::LeftLowerArm, HumanBone::LeftHand, HumanBone::LeftHand, left },
                    { HumanBone::RightShoulder, HumanBone::RightUpperArm, HumanBone::RightUpperArm, -left },
                    { HumanBone::RightUpperArm, HumanBone::RightLowerArm, HumanBone::RightLowerArm, -left },
                    { HumanBone::RightLowerArm, HumanBone::RightHand, HumanBone::RightHand, -left },
                    { HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftLowerLeg, -up },
                    { HumanBone::LeftLowerLeg, HumanBone::LeftFoot, HumanBone::LeftFoot, -up },
                    { HumanBone::LeftFoot, HumanBone::LeftToes, HumanBone::LeftToes, forward },
                    { HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightLowerLeg, -up },
                    { HumanBone::RightLowerLeg, HumanBone::RightFoot, HumanBone::RightFoot, -up },
                    { HumanBone::RightFoot, HumanBone::RightToes, HumanBone::RightToes, forward },
                };
                std::vector<int> target(n, -1);
                std::vector<glm::vec3> wanted(n);
                for (const Bone& b : bones)
                {
                    int from = map[b.From], to = map[b.To] >= 0 ? map[b.To] : map[b.To2];
                    if (from < 0 || to < 0 || from == to) continue;
                    target[from] = to;
                    wanted[from] = b.Dir;
                }
                for (int j : s.Order)
                {
                    int parent = s.Joints[j].Parent;
                    glm::quat inherited = parent >= 0 ? delta[parent] : glm::quat(1, 0, 0, 0);
                    delta[j] = inherited;
                    if (target[j] >= 0)
                    {
                        // The bone as its ancestors' turns left it, turned on to the T-pose's way.
                        glm::vec3 d = glm::vec3(bind[target[j]][3]) - glm::vec3(bind[j][3]);
                        if (glm::dot(d, d) > 1e-12f)
                        {
                            glm::vec3 now = glm::normalize(inherited * d);
                            delta[j] = glm::rotation(now, wanted[j]) * inherited;
                        }
                    }
                    out[j] = glm::normalize(delta[j] * RotationOf(bind[j]));
                }
                return out;
            }
        }

        const char* HumanBoneName(HumanBone bone)
        {
            static const char* names[] = { "Hips", "Spine", "Chest", "Upper Chest", "Neck", "Head",
                                           "Left Shoulder", "Left Upper Arm", "Left Lower Arm", "Left Hand",
                                           "Right Shoulder", "Right Upper Arm", "Right Lower Arm", "Right Hand",
                                           "Left Upper Leg", "Left Lower Leg", "Left Foot", "Left Toes",
                                           "Right Upper Leg", "Right Lower Leg", "Right Foot", "Right Toes" };
            size_t i = (size_t)bone;
            return i < std::size(names) ? names[i] : "";
        }

        HumanoidMap MapHumanoid(const Skeleton& skeleton)
        {
            HumanoidMap map;
            std::vector<int> order = skeleton.Order;
            if (order.size() != skeleton.Joints.size())
            {
                order.clear(); // parents first by depth
                for (int i = 0; i < (int)skeleton.Joints.size(); ++i) order.push_back(i);
                auto depth = [&](int j) {
                    int d = 0;
                    for (int p = skeleton.Joints[j].Parent; p >= 0 && d < 1024; p = skeleton.Joints[p].Parent) ++d;
                    return d;
                };
                std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth(a) < depth(b); });
            }
            // The shallowest joint for each bone (helpers deeper down with the same words lose); spine joints in order.
            std::vector<int> spine;
            int chest = -1;
            for (int j : order)
            {
                NameParts parts = SplitName(skeleton.Joints[j].Name);
                HumanBone bone = Classify(parts);
                if (bone == HumanBone::Count) continue;
                if (bone == HumanBone::Spine)
                {
                    if (Has(parts.Core, "chest") && chest < 0) chest = j;
                    else if (!Has(parts.Core, "chest")) spine.push_back(j);
                    continue;
                }
                int& slot = map.Joints[(size_t)bone];
                if (slot < 0) slot = j;
            }
            // Spine joints below the hips (a rig's "Spine" can also be its root above them).
            int hips = map[HumanBone::Hips];
            auto below = [&](int j, int ancestor) {
                for (int p = skeleton.Joints[j].Parent; p >= 0; p = skeleton.Joints[p].Parent)
                    if (p == ancestor) return true;
                return false;
            };
            if (hips >= 0) std::erase_if(spine, [&](int j) { return !below(j, hips); });
            if (hips < 0 && !spine.empty())
            {
                // No hips by name (Blender's rigs call it "spine"): the first spine joint with legs below it is.
                for (size_t k = 0; k < spine.size() && hips < 0; ++k)
                    if (map[HumanBone::LeftUpperLeg] >= 0 && below(map[HumanBone::LeftUpperLeg], spine[k])) hips = spine[k];
                if (hips >= 0)
                {
                    map.Joints[(size_t)HumanBone::Hips] = hips;
                    std::erase_if(spine, [&](int j) { return j == hips || !below(j, hips); });
                }
            }
            if (!spine.empty()) map.Joints[(size_t)HumanBone::Spine] = spine[0];
            if (chest >= 0) map.Joints[(size_t)HumanBone::Chest] = chest;
            else if (spine.size() >= 2) map.Joints[(size_t)HumanBone::Chest] = spine[1];
            if (spine.size() >= 3 && map[HumanBone::UpperChest] < 0) map.Joints[(size_t)HumanBone::UpperChest] = spine.back();
            return map;
        }

        MuscleLimit HumanMuscleLimit(HumanBone bone)
        {
            auto limit = [](glm::vec2 front, glm::vec2 side, glm::vec2 twist) {
                MuscleLimit m;
                m.Front = front, m.Side = side, m.Twist = twist;
                return m;
            };
            auto bend = [](float most, glm::vec2 twist) {
                MuscleLimit m;
                m.Bend = most, m.Twist = twist;
                return m;
            };
            switch (bone)
            {
            case HumanBone::Spine:
            case HumanBone::Chest: return limit({ -40, 40 }, { -40, 40 }, { -40, 40 });
            case HumanBone::UpperChest: return limit({ -20, 20 }, { -20, 20 }, { -20, 20 });
            case HumanBone::Neck:
            case HumanBone::Head: return limit({ -40, 40 }, { -40, 40 }, { -40, 40 });
            case HumanBone::LeftShoulder:
            case HumanBone::RightShoulder: return limit({ -20, 25 }, { -15, 30 }, { -15, 15 });
            case HumanBone::LeftUpperArm:
            case HumanBone::RightUpperArm: return limit({ -80, 140 }, { -100, 120 }, { -110, 110 });
            case HumanBone::LeftLowerArm:
            case HumanBone::RightLowerArm: return bend(160, { -110, 110 });
            case HumanBone::LeftHand:
            case HumanBone::RightHand: return bend(90, { -90, 90 });
            case HumanBone::LeftUpperLeg:
            case HumanBone::RightUpperLeg: return limit({ -50, 130 }, { -40, 70 }, { -60, 60 });
            case HumanBone::LeftLowerLeg:
            case HumanBone::RightLowerLeg: return limit({ -160, 5 }, { -10, 10 }, { -45, 45 });
            case HumanBone::LeftFoot:
            case HumanBone::RightFoot: return limit({ -60, 40 }, { -35, 35 }, { -35, 35 });
            case HumanBone::LeftToes:
            case HumanBone::RightToes: return limit({ -50, 60 }, { -15, 15 }, { -15, 15 });
            default: return MuscleLimit{}; // the hips: free
            }
        }

        glm::quat ClampMuscle(HumanBone bone, const glm::quat& turn)
        {
            // The bone's way in the T-pose, and the ways its tip swings to for Front and Side (left +X, up +Y, forward +Z).
            glm::vec3 left(1, 0, 0), up(0, 1, 0), forward(0, 0, 1), d = up, front = forward, side = left;
            bool right = bone == HumanBone::RightShoulder || bone == HumanBone::RightUpperArm || bone == HumanBone::RightLowerArm ||
                         bone == HumanBone::RightHand || bone == HumanBone::RightUpperLeg || bone == HumanBone::RightLowerLeg ||
                         bone == HumanBone::RightFoot || bone == HumanBone::RightToes;
            glm::vec3 out = right ? -left : left;
            switch (bone)
            {
            case HumanBone::LeftShoulder: case HumanBone::LeftUpperArm: case HumanBone::LeftLowerArm: case HumanBone::LeftHand:
            case HumanBone::RightShoulder: case HumanBone::RightUpperArm: case HumanBone::RightLowerArm: case HumanBone::RightHand:
                d = out, front = forward, side = up;
                break;
            case HumanBone::LeftUpperLeg: case HumanBone::LeftLowerLeg: case HumanBone::RightUpperLeg: case HumanBone::RightLowerLeg:
                d = -up, front = forward, side = out;
                break;
            case HumanBone::LeftFoot: case HumanBone::LeftToes: case HumanBone::RightFoot: case HumanBone::RightToes:
                d = forward, front = up, side = out;
                break;
            default: break;
            }
            MuscleLimit limit = HumanMuscleLimit(bone);
            glm::quat q = glm::normalize(turn);
            if (q.w < 0.0f) q = -q;
            // Swing (the bone's tip moving) after twist (about the bone): q = swing * twist.
            float twist = 2.0f * std::atan2(glm::dot(glm::vec3(q.x, q.y, q.z), d), q.w);
            glm::quat swing = q * glm::inverse(glm::angleAxis(twist, d));
            if (swing.w < 0.0f) swing = -swing;
            glm::vec3 sv(swing.x, swing.y, swing.z);
            float sl = glm::length(sv);
            glm::vec3 rv = sl > 1e-7f ? sv / sl * (2.0f * std::atan2(sl, swing.w)) : sv * 2.0f; // as a rotation vector
            // A small turn rv moves the tip by rv x d: toward `front` by rv . (d x front).
            glm::vec3 a1 = glm::cross(d, front), a2 = glm::cross(d, side);
            float m1 = glm::dot(rv, a1), m2 = glm::dot(rv, a2);
            auto clamp = [](float v, glm::vec2 range) { return std::clamp(v, glm::radians(range.x), glm::radians(range.y)); };
            float c1 = clamp(m1, limit.Front), c2 = clamp(m2, limit.Side), ct = clamp(twist, limit.Twist);
            float most = glm::radians(limit.Bend), size = std::hypot(c1, c2);
            if (size > most) c1 *= most / size, c2 *= most / size;
            if (c1 == m1 && c2 == m2 && ct == twist) return turn; // within range
            glm::vec3 limited = a1 * c1 + a2 * c2;
            float angle = glm::length(limited);
            glm::quat swung = angle > 1e-7f ? glm::angleAxis(angle, limited / angle) : glm::quat(1, 0, 0, 0);
            return glm::normalize(swung * glm::angleAxis(ct, d));
        }

        AnimationClip RetargetHumanoid(const Skeleton& source, const Skeleton& target, const AnimationClip& clip, bool muscleLimits)
        {
            HumanoidMap from = MapHumanoid(source), to = MapHumanoid(target);
            if (!from.IsHumanoid() || !to.IsHumanoid()) return RetargetClip(source, target, clip);

            // Bind poses in model space.
            Pose sourceBind, targetBind;
            sourceBind.SetBindPose(source);
            targetBind.SetBindPose(target);
            std::vector<glm::mat4> sourceBindG, targetBindG, sourceG, targetG;
            JointGlobals(source, sourceBind, sourceBindG);
            JointGlobals(target, targetBind, targetBindG);
            // Each target joint's human bone source (bones both have), and the turn from its bind pose it takes.
            std::vector<int> sourceOf(target.Joints.size(), -1);
            for (size_t b = 0; b < (size_t)HumanBone::Count; ++b)
                if (from.Joints[b] >= 0 && to.Joints[b] >= 0) sourceOf[to.Joints[b]] = from.Joints[b];
            int sourceHips = from[HumanBone::Hips], targetHips = to[HumanBone::Hips];
            float sourceHeight = std::abs(sourceBindG[sourceHips][3].y), targetHeight = std::abs(targetBindG[targetHips][3].y);
            float scale = sourceHeight > 1e-4f && targetHeight > 1e-4f ? targetHeight / sourceHeight : 1.0f;
            glm::quat rootRotation = RotationOf(target.RootTransform);
            // Motion is measured from each rig's T-pose (so an A-pose rig and a T-pose rig retarget alike).
            std::vector<glm::quat> sourceT = TPoseRotations(source, from, sourceBindG), targetT = TPoseRotations(target, to, targetBindG);
            // Muscle limits: each human bone's turn relative to its parent bone (the nearest both rigs have), in the
            // target's body frame (left, up, forward), kept within range; children keep their own relative turns.
            constexpr size_t boneCount = (size_t)HumanBone::Count;
            std::array<int, boneCount> humanParent;
            std::vector<int> humanOf(target.Joints.size(), -1);
            for (size_t b = 0; b < boneCount; ++b)
            {
                int p = HumanParent((HumanBone)b);
                while (p >= 0 && (from.Joints[p] < 0 || to.Joints[p] < 0)) p = HumanParent((HumanBone)p);
                humanParent[b] = p;
                if (from.Joints[b] >= 0 && to.Joints[b] >= 0) humanOf[to.Joints[b]] = (int)b;
            }
            glm::vec3 bodyLeft = BodyLeft(to, targetBindG);
            glm::quat body = glm::normalize(glm::quat_cast(glm::mat3(bodyLeft, glm::vec3(0, 1, 0), glm::cross(bodyLeft, glm::vec3(0, 1, 0)))));
            std::array<glm::quat, boneCount> sourceTurn, limitedTurn;

            int frames = std::max(2, (int)std::ceil(clip.Duration * 30.0f) + 1);
            std::vector<AnimationChannel> channels(target.Joints.size());
            AnimationChannel hipsTravel;
            hipsTravel.Joint = targetHips;
            hipsTravel.Path = AnimationPath::Translation;
            std::vector<glm::quat> world(target.Joints.size());
            Pose sourcePose, pose;
            for (int f = 0; f < frames; ++f)
            {
                float time = clip.Duration * (float)f / (float)(frames - 1);
                Sample(source, clip, time, sourcePose);
                JointGlobals(source, sourcePose, sourceG);
                // Each human bone's turn from its T-pose (model space), parents before children, limited.
                for (size_t b = 0; b < boneCount; ++b)
                {
                    int s = from.Joints[b];
                    if (s < 0 || to.Joints[b] < 0) continue;
                    sourceTurn[b] = RotationOf(sourceG[s]) * glm::inverse(sourceT[s]);
                    int p = humanParent[b];
                    if (!muscleLimits || p < 0)
                    {
                        limitedTurn[b] = sourceTurn[b];
                        continue;
                    }
                    glm::quat relative = glm::inverse(body) * (glm::inverse(sourceTurn[p]) * sourceTurn[b]) * body;
                    limitedTurn[b] = glm::normalize(limitedTurn[p] * body * ClampMuscle((HumanBone)b, relative) * glm::inverse(body));
                }
                pose = targetBind;
                for (int j : target.Order)
                {
                    int parent = target.Joints[j].Parent;
                    glm::quat parentWorld = parent >= 0 ? world[parent] : rootRotation;
                    if (int s = sourceOf[j]; s >= 0)
                    {
                        // The source bone's turn from its T-pose (model space, limited), on the target bone's T-pose.
                        glm::quat turn = humanOf[j] >= 0 ? limitedTurn[humanOf[j]] : RotationOf(sourceG[s]) * glm::inverse(sourceT[s]);
                        world[j] = glm::normalize(turn * targetT[j]);
                        pose.Rotations[j] = glm::normalize(glm::inverse(parentWorld) * world[j]);
                    }
                    else
                        world[j] = parentWorld * pose.Rotations[j];
                }
                // The hips travel as the source's do, scaled to the target's height.
                JointGlobals(target, pose, targetG);
                glm::vec3 place = glm::vec3(targetBindG[targetHips][3]) + (glm::vec3(sourceG[sourceHips][3]) - glm::vec3(sourceBindG[sourceHips][3])) * scale;
                int hipsParent = target.Joints[targetHips].Parent;
                glm::mat4 parentM = hipsParent >= 0 ? targetG[hipsParent] : target.RootTransform;
                hipsTravel.Times.push_back(time);
                hipsTravel.Values.push_back(glm::vec4(glm::vec3(glm::inverse(parentM) * glm::vec4(place, 1.0f)), 0.0f));
                for (size_t j = 0; j < target.Joints.size(); ++j)
                {
                    if (sourceOf[j] < 0) continue;
                    AnimationChannel& ch = channels[j];
                    glm::quat q = pose.Rotations[j];
                    if (!ch.Values.empty())
                    {
                        glm::vec4 last = ch.Values.back();
                        if (glm::dot(glm::quat(last.w, last.x, last.y, last.z), q) < 0.0f) q = -q; // keep to the short way round
                    }
                    ch.Times.push_back(time);
                    ch.Values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
                }
            }

            AnimationClip out;
            out.Name = clip.Name;
            out.Duration = clip.Duration;
            out.Events = clip.Events;
            out.PropertyChannels = clip.PropertyChannels;
            for (size_t j = 0; j < channels.size(); ++j)
                if (sourceOf[j] >= 0)
                {
                    channels[j].Joint = (int)j;
                    channels[j].Path = AnimationPath::Rotation;
                    out.Channels.push_back(std::move(channels[j]));
                }
            out.Channels.push_back(std::move(hipsTravel));
            // Joints that are no human bone (fingers, tails, props) by name, as plain retargeting does.
            for (AnimationChannel& ch : RetargetClip(source, target, clip).Channels)
                if (sourceOf[ch.Joint] < 0 && ch.Joint != targetHips && ch.Path == AnimationPath::Rotation) out.Channels.push_back(std::move(ch));
            return out;
        }
    }
}
