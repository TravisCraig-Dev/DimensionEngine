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
                    if (to.Parent >= 0 || from.Parent >= 0) continue; // proportions belong to the target
                    float hs = rootHeight(source, ch.Joint), ht = rootHeight(target, t);
                    float scale = hs > 1e-4f && ht > 1e-4f ? ht / hs : 1.0f;
                    for (glm::vec4& v : mapped.Values)
                        v = glm::vec4(to.Translation + (glm::vec3(v) - from.Translation) * scale, v.w);
                }
                else
                    continue; // scale animation stays with the source
                out.Channels.push_back(std::move(mapped));
            }
            return out;
        }
    }
}
