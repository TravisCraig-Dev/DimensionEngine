#include "Dimension/Modeling/Rig.h"
#include "Dimension/Modeling/SparseSolve.h"
#include "Dimension/Modeling/MeshRaycast.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Dimension::Modeling::Rig
{
    namespace
    {
        glm::mat4 PoseDelta(const Bone& b)
        {
            return glm::translate(glm::mat4(1.0f), b.PoseLocation) * glm::mat4_cast(b.PoseRotation) * glm::scale(glm::mat4(1.0f), b.PoseScale);
        }

        void Decompose(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s)
        {
            t = glm::vec3(m[3]);
            s = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            glm::mat3 rot(glm::vec3(m[0]) / std::max(s.x, 1e-12f), glm::vec3(m[1]) / std::max(s.y, 1e-12f), glm::vec3(m[2]) / std::max(s.z, 1e-12f));
            r = glm::normalize(glm::quat_cast(rot));
        }

        // The closest point to p on segment ab.
        glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
        {
            glm::vec3 ab = b - a;
            float l2 = glm::dot(ab, ab);
            float t = l2 > 1e-20f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            return a + ab * t;
        }

        std::vector<glm::vec3> VertexNormals(const EditableMesh& m)
        {
            std::vector<glm::vec3> n(m.VertexCount(), glm::vec3(0.0f));
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
            {
                glm::vec3 fn = m.FaceNormal(f) * m.FaceArea(f);
                for (uint32_t v : m.Faces[f].Verts) n[v] += fn;
            }
            for (auto& x : n)
                if (glm::length(x) > 1e-20f) x = glm::normalize(x);
            return n;
        }

        // Keeps the four largest weights (dropping tiny ones) and scales them to sum to one.
        void Normalize(std::vector<VertexWeight>& w)
        {
            std::sort(w.begin(), w.end(), [](const VertexWeight& a, const VertexWeight& b) { return a.Weight > b.Weight; });
            while (!w.empty() && w.back().Weight <= 1e-4f) w.pop_back();
            if (w.size() > 4) w.resize(4);
            float sum = 0.0f;
            for (auto& x : w) sum += x.Weight;
            if (sum > 0.0f)
                for (auto& x : w) x.Weight /= sum;
        }

        std::vector<glm::vec3> Mirrors(uint8_t symmetry)
        {
            std::vector<glm::vec3> out;
            for (int bits = 0; bits < 8; ++bits)
                if ((bits & ~symmetry) == 0) out.push_back(glm::vec3(bits & 1 ? -1.0f : 1.0f, bits & 2 ? -1.0f : 1.0f, bits & 4 ? -1.0f : 1.0f));
            return out;
        }

        void EnsureWeights(EditableMesh& m)
        {
            if (m.Weights.size() != m.VertexCount()) m.Weights.resize(m.VertexCount());
        }
    }

    // ---- Bones

    glm::mat4 RestMatrix(const Bone& b)
    {
        glm::vec3 d = b.Tail - b.Head;
        float len = glm::length(d);
        glm::vec3 y = len > 1e-12f ? d / len : glm::vec3(0, 1, 0);
        // The shortest turn taking +Y onto the bone, then the roll about it.
        glm::quat q;
        float c = y.y;
        if (c > 0.99999f) q = glm::quat(1, 0, 0, 0);
        else if (c < -0.99999f) q = glm::angleAxis(glm::pi<float>(), glm::vec3(0, 0, 1));
        else q = glm::angleAxis(std::acos(std::clamp(c, -1.0f, 1.0f)), glm::normalize(glm::cross(glm::vec3(0, 1, 0), y)));
        q = q * glm::angleAxis(b.Roll, glm::vec3(0, 1, 0));
        return glm::translate(glm::mat4(1.0f), b.Head) * glm::mat4_cast(q);
    }

    glm::mat4 RestLocal(const EditableMesh& m, int bone)
    {
        const Bone& b = m.Bones[(size_t)bone];
        glm::mat4 rest = RestMatrix(b);
        if (b.Parent < 0 || b.Parent >= (int)m.Bones.size()) return rest;
        return glm::inverse(RestMatrix(m.Bones[(size_t)b.Parent])) * rest;
    }

    std::vector<glm::mat4> PoseMatrices(const EditableMesh& m)
    {
        size_t n = m.Bones.size();
        std::vector<glm::mat4> out(n, glm::mat4(1.0f));
        std::vector<uint8_t> done(n, 0);
        std::vector<int> stack;
        for (size_t i = 0; i < n; ++i)
        {
            // Parents first (walk up to the first done ancestor).
            stack.clear();
            for (int b = (int)i; b >= 0 && b < (int)n && !done[(size_t)b] && stack.size() <= n; b = m.Bones[(size_t)b].Parent) stack.push_back(b);
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            {
                int b = *it;
                const Bone& bone = m.Bones[(size_t)b];
                glm::mat4 local = RestLocal(m, b) * PoseDelta(bone);
                bool hasParent = bone.Parent >= 0 && bone.Parent < (int)n && done[(size_t)bone.Parent];
                out[(size_t)b] = hasParent ? out[(size_t)bone.Parent] * local : local;
                done[(size_t)b] = 1;
            }
        }
        return out;
    }

    std::vector<glm::mat4> SkinMatrices(const EditableMesh& m)
    {
        std::vector<glm::mat4> pose = PoseMatrices(m), rest = RestMatrices(m);
        std::vector<glm::mat4> out(pose.size());
        for (size_t b = 0; b < pose.size(); ++b) out[b] = pose[b] * glm::inverse(rest[b]);
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            if (m.Bones[b].Segments <= 1) continue;
            std::vector<glm::mat4> posed = BBoneSegments(m, (int)b, pose), restSeg = BBoneSegments(m, (int)b, rest);
            for (size_t k = 0; k < posed.size(); ++k) out.push_back(posed[k] * glm::inverse(restSeg[k]));
        }
        return out;
    }

    std::vector<glm::mat4> RestMatrices(const EditableMesh& m)
    {
        std::vector<glm::mat4> out(m.Bones.size());
        for (size_t b = 0; b < m.Bones.size(); ++b) out[b] = RestMatrix(m.Bones[b]);
        return out;
    }

    // ---- B-bones

    namespace
    {
        bool Joined(const glm::vec3& a, const glm::vec3& b, const Bone& scale)
        {
            return glm::length(a - b) <= std::max(glm::length(scale.Tail - scale.Head) * 1e-3f, 1e-5f);
        }
    }

    int BBoneInBone(const EditableMesh& m, int bone)
    {
        const Bone& b = m.Bones[(size_t)bone];
        if (b.Parent < 0 || b.Parent >= (int)m.Bones.size()) return -1;
        return Joined(m.Bones[(size_t)b.Parent].Tail, b.Head, b) ? b.Parent : -1;
    }

    int BBoneOutBone(const EditableMesh& m, int bone)
    {
        const Bone& b = m.Bones[(size_t)bone];
        for (size_t c = 0; c < m.Bones.size(); ++c)
            if (m.Bones[c].Parent == bone && Joined(m.Bones[c].Head, b.Tail, b)) return (int)c;
        return -1;
    }

    std::vector<glm::mat4> BBoneSegments(const EditableMesh& m, int bone, const std::vector<glm::mat4>& frames)
    {
        const Bone& b = m.Bones[(size_t)bone];
        const glm::mat4& F = frames[(size_t)bone];
        int n = std::clamp(b.Segments, 1, 32);
        float len = glm::length(b.Tail - b.Head);
        if (n <= 1 || len < 1e-8f) return { F };
        // The curve in the bone's own frame (unscaled: the frame's scale applies on top), +Y from head to tail.
        glm::mat3 toLocal = glm::inverse(glm::mat3(F));
        auto localDir = [&](const glm::vec3& v) {
            glm::vec3 d = toLocal * v;
            return glm::length(d) > 1e-12f ? glm::normalize(d) : glm::vec3(0, 1, 0);
        };
        int in = BBoneInBone(m, bone), out = BBoneOutBone(m, bone);
        glm::vec3 dIn = in >= 0 ? localDir(glm::vec3(frames[(size_t)in][1])) : glm::vec3(0, 1, 0);
        glm::vec3 dOut = out >= 0 ? localDir(glm::vec3(frames[(size_t)out][1])) : glm::vec3(0, 1, 0);
        glm::vec3 p0(0.0f), p3(0.0f, len, 0.0f);
        glm::vec3 p1 = p0 + dIn * (len * std::max(b.EaseIn, 0.0f) / 3.0f), p2 = p3 - dOut * (len * std::max(b.EaseOut, 0.0f) / 3.0f);
        auto at = [&](float t) {
            float s = 1.0f - t;
            return p0 * (s * s * s) + p1 * (3.0f * s * s * t) + p2 * (3.0f * s * t * t) + p3 * (t * t * t);
        };
        std::vector<glm::vec3> q((size_t)n + 1);
        for (int k = 0; k <= n; ++k) q[(size_t)k] = at((float)k / (float)n);
        // Frames carried along the chords without twisting (each turned by the least rotation onto the next chord).
        std::vector<glm::vec3> tangent((size_t)n), xAxis((size_t)n);
        for (int k = 0; k < n; ++k)
        {
            glm::vec3 d = q[(size_t)k + 1] - q[(size_t)k];
            tangent[(size_t)k] = glm::length(d) > 1e-12f ? glm::normalize(d) : glm::vec3(0, 1, 0);
        }
        auto turn = [](const glm::vec3& v, const glm::vec3& from, const glm::vec3& to) {
            glm::vec3 axis = glm::cross(from, to);
            float s = glm::length(axis), c = glm::dot(from, to);
            if (s < 1e-9f) return v;
            return glm::vec3(glm::mat4_cast(glm::angleAxis(std::atan2(s, c), axis / s)) * glm::vec4(v, 0.0f));
        };
        xAxis[0] = turn(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), tangent[0]);
        for (int k = 1; k < n; ++k) xAxis[(size_t)k] = turn(xAxis[(size_t)k - 1], tangent[(size_t)k - 1], tangent[(size_t)k]);
        // The twist: the child's X about the end, against the carried frame's, spread along the segments.
        float twist = 0.0f;
        if (out >= 0)
        {
            glm::vec3 endT = tangent[(size_t)n - 1];
            glm::vec3 carried = turn(xAxis[(size_t)n - 1], endT, dOut);
            glm::vec3 childX = toLocal * glm::vec3(frames[(size_t)out][0]);
            childX -= dOut * glm::dot(childX, dOut);
            if (glm::length(childX) > 1e-9f)
            {
                childX = glm::normalize(childX);
                twist = std::atan2(glm::dot(glm::cross(carried, childX), dOut), glm::dot(carried, childX));
            }
        }
        std::vector<glm::mat4> segs((size_t)n);
        float share = len / (float)n;
        for (int k = 0; k < n; ++k)
        {
            glm::vec3 y = tangent[(size_t)k];
            float angle = twist * ((float)k + 0.5f) / (float)n;
            glm::vec3 x = glm::vec3(glm::mat4_cast(glm::angleAxis(angle, y)) * glm::vec4(xAxis[(size_t)k], 0.0f));
            x = glm::normalize(x - y * glm::dot(x, y));
            glm::vec3 z = glm::cross(x, y);
            float stretch = glm::length(q[(size_t)k + 1] - q[(size_t)k]) / share;
            glm::mat4 local(1.0f);
            local[0] = glm::vec4(x, 0.0f);
            local[1] = glm::vec4(y * stretch, 0.0f);
            local[2] = glm::vec4(z, 0.0f);
            local[3] = glm::vec4(q[(size_t)k], 1.0f);
            segs[(size_t)k] = F * local;
        }
        return segs;
    }

    SkinLayout MakeSkinLayout(const EditableMesh& m)
    {
        SkinLayout l;
        l.First.assign(m.Bones.size(), -1);
        l.RestCurve.resize(m.Bones.size());
        l.JointCount = m.Bones.size();
        std::vector<glm::mat4> rest;
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            if (m.Bones[b].Segments <= 1) continue;
            if (rest.empty()) rest = RestMatrices(m);
            std::vector<glm::mat4> segs = BBoneSegments(m, (int)b, rest);
            if (segs.size() <= 1) continue;
            l.First[b] = (int)l.JointCount;
            l.JointCount += segs.size();
            for (const glm::mat4& s : segs) l.RestCurve[b].push_back(glm::vec3(s[3]));
            l.RestCurve[b].push_back(m.Bones[b].Tail);
        }
        return l;
    }

    void SkinLayout::Joints(const glm::vec3& p, const std::vector<VertexWeight>& bones, std::vector<VertexWeight>& joints) const
    {
        joints.clear();
        auto add = [&](uint32_t joint, float w) {
            if (w <= 0.0f) return;
            for (VertexWeight& j : joints)
                if (j.Bone == joint)
                {
                    j.Weight += w;
                    return;
                }
            joints.push_back({ (uint16_t)joint, w });
        };
        for (const VertexWeight& bw : bones)
        {
            if (bw.Bone >= First.size() || First[bw.Bone] < 0)
            {
                add(bw.Bone, bw.Weight);
                continue;
            }
            // Where along the rest curve the vertex lies (segments 0..n), each segment's joint counted at its middle.
            const std::vector<glm::vec3>& c = RestCurve[bw.Bone];
            size_t n = c.size() - 1;
            float best = 1e30f, u = 0.0f;
            for (size_t k = 0; k < n; ++k)
            {
                glm::vec3 ab = c[k + 1] - c[k];
                float l2 = glm::dot(ab, ab);
                float t = l2 > 1e-20f ? std::clamp(glm::dot(p - c[k], ab) / l2, 0.0f, 1.0f) : 0.0f;
                float d = glm::length(p - (c[k] + ab * t));
                if (d < best - 1e-7f)
                {
                    best = d;
                    u = (float)k + t;
                }
            }
            float x = std::clamp(u - 0.5f, 0.0f, (float)n - 1.0f);
            size_t j = std::min((size_t)x, n - 1);
            float f = x - (float)j;
            add((uint32_t)(First[bw.Bone] + (int)j), bw.Weight * (1.0f - f));
            if (j + 1 < n) add((uint32_t)(First[bw.Bone] + (int)j + 1), bw.Weight * f);
        }
        Normalize(joints);
    }

    Ref<Skeleton> BuildSkeleton(const EditableMesh& m)
    {
        auto s = MakeRef<Skeleton>();
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            Joint j;
            j.Name = m.Bones[b].Name;
            j.Parent = m.Bones[b].Parent >= 0 && m.Bones[b].Parent < (int)m.Bones.size() ? m.Bones[b].Parent : -1;
            Decompose(RestLocal(m, (int)b), j.Translation, j.Rotation, j.Scale);
            j.InverseBind = glm::inverse(RestMatrix(m.Bones[b]));
            s->Joints.push_back(j);
        }
        // B-bone segments: children of their bone (actions bake their bending; at rest they ride along with it).
        std::vector<glm::mat4> rest = RestMatrices(m);
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            if (m.Bones[b].Segments <= 1) continue;
            std::vector<glm::mat4> segs = BBoneSegments(m, (int)b, rest);
            if (segs.size() <= 1) continue;
            for (size_t k = 0; k < segs.size(); ++k)
            {
                Joint j;
                j.Name = m.Bones[b].Name + ".seg" + std::to_string(k);
                j.Parent = (int)b;
                Decompose(glm::inverse(rest[b]) * segs[k], j.Translation, j.Rotation, j.Scale);
                j.InverseBind = glm::inverse(segs[k]);
                s->Joints.push_back(j);
            }
        }
        s->ComputeOrder();
        return s;
    }

    void PoseMeshData(MeshData& data, const EditableMesh& m)
    {
        if (data.Skin.size() != data.Vertices.size() || m.Bones.empty()) return;
        std::vector<glm::mat4> skin = SkinMatrices(m);
        for (size_t i = 0; i < data.Vertices.size(); ++i)
        {
            const SkinVertex& s = data.Skin[i];
            glm::mat4 x(0.0f);
            for (int k = 0; k < 4; ++k)
                if (s.Weights[k] > 0.0f && s.Joints[k] < skin.size()) x += skin[s.Joints[k]] * s.Weights[k];
            Vertex& v = data.Vertices[i];
            v.Position = glm::vec3(x * glm::vec4(v.Position, 1.0f));
            glm::vec3 n = glm::mat3(x) * v.Normal;
            if (glm::length(n) > 1e-20f) v.Normal = glm::normalize(n);
            glm::vec3 t = glm::mat3(x) * glm::vec3(v.Tangent);
            if (v.Tangent.w != 0.0f && glm::length(t) > 1e-20f) v.Tangent = glm::vec4(glm::normalize(t), v.Tangent.w);
        }
        data.Skin.clear();
        data.SkeletonData = nullptr;
    }

    bool AtRest(const EditableMesh& m)
    {
        for (const Bone& b : m.Bones)
            if (b.PoseLocation != glm::vec3(0.0f) || b.PoseScale != glm::vec3(1.0f) || std::abs(b.PoseRotation.w) < 0.999999f) return false;
        return true;
    }

    void ClearPose(EditableMesh& m, bool selectedOnly)
    {
        for (Bone& b : m.Bones)
        {
            if (selectedOnly && !(b.Flags & BoneBody)) continue;
            b.PoseRotation = glm::quat(1, 0, 0, 0);
            b.PoseLocation = glm::vec3(0.0f);
            b.PoseScale = glm::vec3(1.0f);
        }
    }

    std::string FlipSideName(const std::string& name)
    {
        struct Pair
        {
            const char *A, *B;
        };
        static const Pair suffixes[] = { { ".L", ".R" }, { "_L", "_R" }, { ".l", ".r" }, { "_l", "_r" }, { "Left", "Right" }, { "left", "right" } };
        for (const Pair& p : suffixes)
            for (int side = 0; side < 2; ++side)
            {
                std::string from = side ? p.B : p.A, to = side ? p.A : p.B;
                if (name.size() > from.size() && name.compare(name.size() - from.size(), from.size(), from) == 0)
                    return name.substr(0, name.size() - from.size()) + to;
            }
        static const Pair prefixes[] = { { "L_", "R_" }, { "Left", "Right" }, { "left", "right" } };
        for (const Pair& p : prefixes)
            for (int side = 0; side < 2; ++side)
            {
                std::string from = side ? p.B : p.A, to = side ? p.A : p.B;
                if (name.size() > from.size() && name.compare(0, from.size(), from) == 0) return to + name.substr(from.size());
            }
        return name;
    }

    int FindBone(const EditableMesh& m, const std::string& name)
    {
        for (size_t b = 0; b < m.Bones.size(); ++b)
            if (m.Bones[b].Name == name) return (int)b;
        return -1;
    }

    std::string UniqueBoneName(const EditableMesh& m, const std::string& base)
    {
        if (FindBone(m, base) < 0) return base;
        for (int i = 1;; ++i)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), ".%03d", i);
            if (FindBone(m, base + buf) < 0) return base + buf;
        }
    }

    int AddBone(EditableMesh& m, const std::string& name, const glm::vec3& head, const glm::vec3& tail, int parent)
    {
        Bone b;
        b.Name = UniqueBoneName(m, name.empty() ? "Bone" : name);
        b.Head = head;
        b.Tail = tail;
        b.Parent = parent >= 0 && parent < (int)m.Bones.size() ? parent : -1;
        float len = glm::length(tail - head);
        if (len > 1e-6f)
        {
            // (Blender's envelope for a 1 m bone, scaled to this one)
            b.HeadRadius = 0.1f * len;
            b.TailRadius = 0.05f * len;
            b.EnvelopeDistance = 0.25f * len;
        }
        m.Bones.push_back(b);
        int index = (int)m.Bones.size() - 1;
        for (Action& a : m.Actions)
            for (ActionKey& k : a.Keys)
                if (k.Rotations.size() == (size_t)index)
                {
                    k.Rotations.push_back(glm::quat(1, 0, 0, 0));
                    k.Locations.push_back(glm::vec3(0.0f));
                    k.Scales.push_back(glm::vec3(1.0f));
                    if (!k.Keyed.empty()) k.Keyed.push_back(0);
                    if (!k.Curves.empty()) k.Curves.resize(k.Rotations.size());
                }
        return index;
    }

    bool WouldLoop(const EditableMesh& m, int bone, int parent)
    {
        for (int p = parent, steps = 0; p >= 0 && p < (int)m.Bones.size() && steps <= (int)m.Bones.size(); p = m.Bones[(size_t)p].Parent, ++steps)
            if (p == bone) return true;
        return false;
    }

    void DeleteBones(EditableMesh& m, const std::vector<int>& bones)
    {
        size_t n = m.Bones.size();
        std::vector<uint8_t> gone(n, 0);
        for (int b : bones)
            if (b >= 0 && b < (int)n) gone[(size_t)b] = 1;
        std::vector<int> remap(n, -1);
        int next = 0;
        for (size_t b = 0; b < n; ++b)
            if (!gone[b]) remap[b] = next++;
        auto survivingParent = [&](int p) {
            while (p >= 0 && p < (int)n && gone[(size_t)p]) p = m.Bones[(size_t)p].Parent;
            return p >= 0 && p < (int)n ? remap[(size_t)p] : -1;
        };
        std::vector<Bone> kept;
        for (size_t b = 0; b < n; ++b)
        {
            if (gone[b]) continue;
            Bone bone = m.Bones[b];
            bone.Parent = survivingParent(bone.Parent);
            bone.IKTarget = bone.IKTarget >= 0 && bone.IKTarget < (int)n ? remap[(size_t)bone.IKTarget] : -1;
            kept.push_back(bone);
        }
        for (auto& list : m.Weights)
        {
            std::vector<VertexWeight> out;
            for (const VertexWeight& w : list)
                if (w.Bone < n && remap[w.Bone] >= 0) out.push_back({ (uint16_t)remap[w.Bone], w.Weight });
            list = std::move(out);
        }
        for (Action& a : m.Actions)
            for (ActionKey& k : a.Keys)
            {
                ActionKey out;
                out.Time = k.Time;
                out.Shapes = k.Shapes;
                for (size_t b = 0; b < n && b < k.Rotations.size(); ++b)
                    if (!gone[b])
                    {
                        out.Rotations.push_back(k.Rotations[b]);
                        out.Locations.push_back(k.Locations[b]);
                        out.Scales.push_back(k.Scales[b]);
                        if (!k.Keyed.empty()) out.Keyed.push_back(b < k.Keyed.size() ? k.Keyed[b] : 0);
                        if (!k.Curves.empty()) out.Curves.push_back(k.CurveOf(b));
                    }
                k = std::move(out);
            }
        m.Bones = std::move(kept);
    }

    int Symmetrize(EditableMesh& m)
    {
        int made = 0;
        size_t n = m.Bones.size();
        std::vector<int> mirrorOf(n, -1);
        for (size_t b = 0; b < n; ++b)
        {
            const Bone& bone = m.Bones[b];
            std::string flipped = FlipSideName(bone.Name);
            if (flipped == bone.Name || bone.Head.x < -1e-6f || bone.Tail.x < -1e-6f) continue; // centered or on -X
            int existing = FindBone(m, flipped);
            glm::vec3 head = bone.Head * glm::vec3(-1, 1, 1), tail = bone.Tail * glm::vec3(-1, 1, 1);
            if (existing >= 0)
            {
                m.Bones[(size_t)existing].Head = head;
                m.Bones[(size_t)existing].Tail = tail;
                m.Bones[(size_t)existing].Roll = -bone.Roll;
                mirrorOf[b] = existing;
                continue;
            }
            int nb = AddBone(m, flipped, head, tail, -1);
            m.Bones[(size_t)nb].Roll = -bone.Roll;
            m.Bones[(size_t)nb].Deform = bone.Deform;
            mirrorOf[b] = nb;
            ++made;
        }
        // Parents: the mirrored bone's parent is the mirror of the original's (or the same centered bone).
        for (size_t b = 0; b < n; ++b)
        {
            if (mirrorOf[b] < 0) continue;
            int p = m.Bones[b].Parent;
            int mp = p >= 0 && p < (int)n && mirrorOf[(size_t)p] >= 0 ? mirrorOf[(size_t)p] : p;
            if (!WouldLoop(m, mirrorOf[b], mp)) m.Bones[(size_t)mirrorOf[b]].Parent = mp;
        }
        return made;
    }

    // ---- Weights

    float GetWeight(const EditableMesh& m, uint32_t v, int bone)
    {
        if (v >= m.Weights.size()) return 0.0f;
        for (const VertexWeight& w : m.Weights[v])
            if (w.Bone == bone) return w.Weight;
        return 0.0f;
    }

    void SetWeight(EditableMesh& m, uint32_t v, int bone, float weight)
    {
        EnsureWeights(m);
        auto& list = m.Weights[v];
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].Bone == bone)
            {
                if (weight <= 0.0f) list.erase(list.begin() + (std::ptrdiff_t)i);
                else list[i].Weight = std::min(weight, 1.0f);
                return;
            }
        if (weight > 0.0f) list.push_back({ (uint16_t)bone, std::min(weight, 1.0f) });
    }

    void NormalizeWeights(EditableMesh& m)
    {
        for (auto& list : m.Weights)
        {
            std::vector<VertexWeight> deform;
            for (const VertexWeight& w : list)
                if (w.Bone < m.Bones.size() && m.Bones[w.Bone].Deform) deform.push_back(w);
            Normalize(deform);
            list = std::move(deform);
        }
    }

    void AutomaticWeights(EditableMesh& m, bool selectedOnly)
    {
        std::vector<int> deform;
        for (size_t b = 0; b < m.Bones.size(); ++b)
            if (m.Bones[b].Deform) deform.push_back((int)b);
        if (deform.empty() || m.VertexCount() == 0) return;
        EnsureWeights(m);
        MeshRaycaster rays;
        rays.Build(m);
        std::vector<glm::vec3> normals = VertexNormals(m);
        AABB bounds = m.Bounds();
        float size = std::max(glm::length(bounds.Max - bounds.Min), 1e-4f), eps = size * 1e-3f;
        uint32_t n = m.VertexCount();
        std::vector<uint8_t> chosen(n, 1);
        if (selectedOnly)
            for (uint32_t v = 0; v < n; ++v) chosen[v] = m.IsVertSelected(v);
        // Bone heat (Baran and Popovic, "Automatic Rigging and Animation of 3D Characters"): each bone's weights are
        // heat diffusing over the surface, (L + A H) w = A H p, with L the cotangent Laplacian, A the vertices' areas,
        // H_j = 1 / d_j^2 for d_j the distance to the nearest bone vertex j can see, and p_j = 1 where that bone is
        // this one. The matrix is the same for every bone: factored once.
        std::vector<float> heat(n, 0.0f);
        std::vector<std::vector<int>> nearestBones(n);
        for (uint32_t v = 0; v < n; ++v)
        {
            glm::vec3 p = m.Positions[v], inside = p - normals[v] * eps;
            float bestSeen = 1e30f, bestAny = 1e30f;
            int any = -1;
            std::vector<std::pair<float, int>> seen;
            for (int b : deform)
            {
                const Bone& bone = m.Bones[(size_t)b];
                glm::vec3 q = ClosestOnSegment(p, bone.Head, bone.Tail);
                float d = std::max(glm::length(p - q), eps);
                if (d < bestAny)
                {
                    bestAny = d;
                    any = b;
                }
                // Hidden behind a wall of the mesh (another part of the body)? Seen when any of a few points along the
                // bone is (the nearest one can sit on the surface where a bone pokes out).
                bool visible = false;
                for (float k : { -1.0f, 0.0f, 0.25f, 0.5f, 0.75f })
                    if (!visible) visible = !rays.Occluded(inside, k < 0.0f ? q : glm::mix(bone.Head, bone.Tail, k));
                if (!visible) continue;
                seen.push_back({ d, b });
                bestSeen = std::min(bestSeen, d);
            }
            if (seen.empty())
            {
                // (no bone in sight: the nearest one, weakly, so the neighbours' heat mostly decides)
                if (any >= 0) nearestBones[v].push_back(any);
                heat[v] = 0.1f / (bestAny * bestAny);
                continue;
            }
            for (auto& [d, b] : seen)
                if (d <= bestSeen * 1.0001f) nearestBones[v].push_back(b);
            heat[v] = 1.0f / (bestSeen * bestSeen);
        }
        // Cotangent weights and vertex areas over the triangulated faces.
        Sparse::SymmetricSystem system;
        system.Rows.resize(n);
        std::vector<double> area(n, 0.0);
        std::vector<uint32_t> tri;
        for (uint32_t f = 0; f < m.FaceCount(); ++f)
        {
            const Face& face = m.Faces[f];
            tri.clear();
            m.TriangulateFace(f, tri);
            for (size_t k = 0; k + 2 < tri.size(); k += 3)
            {
                uint32_t id[3] = { face.Verts[tri[k]], face.Verts[tri[k + 1]], face.Verts[tri[k + 2]] };
                glm::dvec3 p[3] = { glm::dvec3(m.Positions[id[0]]), glm::dvec3(m.Positions[id[1]]), glm::dvec3(m.Positions[id[2]]) };
                double a = glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) * 0.5;
                if (a < 1e-20) continue;
                for (int c = 0; c < 3; ++c)
                {
                    area[id[c]] += a / 3.0;
                    // The angle at corner c weighs the opposite edge.
                    glm::dvec3 u = p[(c + 1) % 3] - p[c], w = p[(c + 2) % 3] - p[c];
                    double cot = std::clamp(glm::dot(u, w) / std::max(glm::length(glm::cross(u, w)), 1e-20), -10.0, 1e3) * 0.5;
                    uint32_t i = id[(c + 1) % 3], j = id[(c + 2) % 3];
                    system.Add(i, i, cot);
                    system.Add(j, j, cot);
                    system.Add(i, j, -cot);
                    system.Add(j, i, -cot);
                }
            }
        }
        double meanArea = 0.0;
        for (double a : area) meanArea += a;
        meanArea = std::max(meanArea / (double)n, 1e-20);
        std::vector<double> diag(n);
        for (uint32_t v = 0; v < n; ++v)
        {
            double a = area[v] > 0.0 ? area[v] : meanArea;
            diag[v] = a * (double)heat[v] + meanArea * 1e-9 * (double)heat[v] + 1e-12;
            system.Add(v, v, diag[v]);
        }
        Sparse::EnvelopeCholesky chol;
        bool direct = chol.Factor(system);
        std::vector<std::vector<VertexWeight>> w(n);
        std::vector<double> rhs(n), x(n);
        for (int b : deform)
        {
            bool any = false;
            for (uint32_t v = 0; v < n; ++v)
            {
                double share = 0.0;
                for (int nb : nearestBones[v])
                    if (nb == b) share = 1.0 / (double)nearestBones[v].size();
                rhs[v] = diag[v] * share;
                any |= share > 0.0;
            }
            if (!any) continue;
            if (direct) chol.Solve(rhs, x);
            else
            {
                std::fill(x.begin(), x.end(), 0.0);
                system.Solve(rhs, x, (int)std::clamp<size_t>(n * 2, 100, 5000), 1e-8);
            }
            for (uint32_t v = 0; v < n; ++v)
                if (chosen[v] && x[v] > 0.005) w[v].push_back({ (uint16_t)b, (float)std::min(x[v], 1.0) });
        }
        for (uint32_t v = 0; v < n; ++v)
        {
            if (!chosen[v]) continue;
            if (w[v].empty() && !nearestBones[v].empty()) w[v].push_back({ (uint16_t)nearestBones[v][0], 1.0f });
            Normalize(w[v]);
            m.Weights[v] = w[v];
        }
    }

    float EnvelopeInfluence(const Bone& b, const glm::vec3& p)
    {
        glm::vec3 ab = b.Tail - b.Head;
        float l2 = glm::dot(ab, ab);
        float t = l2 > 1e-20f ? std::clamp(glm::dot(p - b.Head, ab) / l2, 0.0f, 1.0f) : 0.0f;
        float d = glm::length(p - (b.Head + ab * t));
        float radius = std::max(0.0f, b.HeadRadius + (b.TailRadius - b.HeadRadius) * t);
        if (d <= radius) return b.EnvelopeWeight;
        float dist = std::max(b.EnvelopeDistance, 0.0f);
        if (dist <= 0.0f || d >= radius + dist) return 0.0f;
        float f = 1.0f - (d - radius) / dist;
        return b.EnvelopeWeight * f * f; // (smooth to nothing at the envelope's edge)
    }

    size_t EnvelopeWeights(EditableMesh& m, bool selectedOnly)
    {
        EnsureWeights(m);
        size_t count = 0;
        for (uint32_t v = 0; v < m.VertexCount(); ++v)
        {
            if (selectedOnly && !m.IsVertSelected(v)) continue;
            std::vector<VertexWeight> w;
            for (size_t b = 0; b < m.Bones.size(); ++b)
            {
                if (!m.Bones[b].Deform) continue;
                float x = EnvelopeInfluence(m.Bones[b], m.Positions[v]);
                if (x > 0.0f) w.push_back({ (uint16_t)b, x });
            }
            Normalize(w);
            if (w.empty()) continue;
            m.Weights[v] = std::move(w);
            ++count;
        }
        return count;
    }

    std::vector<std::vector<VertexWeight>> TransferWeights(const EditableMesh& from, const EditableMesh& to)
    {
        std::vector<std::vector<VertexWeight>> out(to.VertexCount());
        if (from.Weights.size() != from.VertexCount() || from.VertexCount() == 0) return out;
        // Source points, with mirrored copies for the Mirror modifiers (their bones flipped by name).
        struct Src
        {
            glm::vec3 P;
            uint32_t Vertex;
            bool Flip;
        };
        std::vector<Src> pts;
        uint8_t axes = 0;
        for (const Modifier& mod : from.Modifiers)
            if (mod.Enabled && mod.Type == ModifierType::Mirror)
                for (int a = 0; a < 3; ++a)
                    if (mod.Axis[a]) axes |= (uint8_t)(1 << a);
        for (const glm::vec3& s : Mirrors(axes))
        {
            bool flip = (s.x * s.y * s.z) < 0.0f;
            for (uint32_t v = 0; v < from.VertexCount(); ++v) pts.push_back({ from.Positions[v] * s, v, flip });
        }
        std::vector<int> flipped(from.Bones.size());
        for (size_t b = 0; b < from.Bones.size(); ++b)
        {
            int f = FindBone(from, FlipSideName(from.Bones[b].Name));
            flipped[b] = f >= 0 ? f : (int)b;
        }
        // A grid over the points for the nearest ones.
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (const Src& p : pts)
        {
            mn = glm::min(mn, p.P);
            mx = glm::max(mx, p.P);
        }
        float cell = std::max(glm::length(mx - mn) / 48.0f, 1e-5f);
        auto key = [&](const glm::ivec3& c) { return ((int64_t)c.x * 73856093ll) ^ ((int64_t)c.y * 19349663ll) ^ ((int64_t)c.z * 83492791ll); };
        std::unordered_map<int64_t, std::vector<uint32_t>> grid;
        for (uint32_t i = 0; i < pts.size(); ++i) grid[key(glm::ivec3(glm::floor((pts[i].P - mn) / cell)))].push_back(i);
        for (uint32_t v = 0; v < to.VertexCount(); ++v)
        {
            glm::vec3 p = to.Positions[v];
            glm::ivec3 c = glm::ivec3(glm::floor((p - mn) / cell));
            std::vector<std::pair<float, uint32_t>> near;
            for (int r = 1; r <= 6 && near.size() < 4; ++r)
            {
                near.clear();
                for (int dz = -r; dz <= r; ++dz)
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx)
                            if (auto it = grid.find(key(c + glm::ivec3(dx, dy, dz))); it != grid.end())
                                for (uint32_t i : it->second) near.push_back({ glm::length(pts[i].P - p), i });
            }
            if (near.empty())
                for (uint32_t i = 0; i < pts.size(); ++i) near.push_back({ glm::length(pts[i].P - p), i });
            size_t k = std::min<size_t>(4, near.size());
            std::partial_sort(near.begin(), near.begin() + (std::ptrdiff_t)k, near.end());
            std::unordered_map<uint16_t, float> sum;
            for (size_t i = 0; i < k; ++i)
            {
                const Src& s = pts[near[i].second];
                float w = 1.0f / (near[i].first * near[i].first + 1e-10f);
                if (near[i].first < 1e-6f) w = 1e12f; // on a source vertex: that one
                for (const VertexWeight& x : from.Weights[s.Vertex])
                    sum[(uint16_t)(s.Flip && x.Bone < flipped.size() ? flipped[x.Bone] : x.Bone)] += x.Weight * w;
            }
            for (auto& [b, x] : sum) out[v].push_back({ b, x });
            Normalize(out[v]);
        }
        return out;
    }

    size_t PaintWeights(EditableMesh& m, const glm::vec3& center, const WeightBrush& brush)
    {
        if (brush.Bone < 0 || brush.Bone >= (int)m.Bones.size()) return 0;
        EnsureWeights(m);
        std::vector<glm::vec3> normals = VertexNormals(m);
        const Topology& t = m.Topo();
        size_t changed = 0;
        for (const glm::vec3& s : Mirrors(brush.Symmetry))
        {
            glm::vec3 c = center * s, view = brush.ViewDir * s;
            int bone = brush.Bone;
            if (s.x * s.y * s.z < 0.0f)
                if (int f = FindBone(m, FlipSideName(m.Bones[(size_t)bone].Name)); f >= 0) bone = f;
            std::vector<std::pair<uint32_t, float>> next;
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                if (m.IsVertHidden(v)) continue;
                float d = glm::length(m.Positions[v] - c);
                if (d >= brush.Radius) continue;
                if (brush.FrontFacesOnly && glm::dot(normals[v], view) > 0.0f) continue;
                float tt = d / brush.Radius, f = (1.0f - tt * tt * (3.0f - 2.0f * tt)) * std::clamp(brush.Strength, 0.0f, 1.0f);
                float w = GetWeight(m, v, bone), target = w;
                switch (brush.Mode)
                {
                case WeightMode::Add: target = w + brush.Value * f; break;
                case WeightMode::Subtract: target = w - brush.Value * f; break;
                case WeightMode::Mix: target = w + (brush.Value - w) * f; break;
                case WeightMode::Smooth:
                {
                    if (v >= t.VertEdges.size()) break;
                    float sum = 0.0f;
                    for (uint32_t e : t.VertEdges[v])
                        sum += GetWeight(m, EdgeA(t.Edges[e]) == v ? EdgeB(t.Edges[e]) : EdgeA(t.Edges[e]), bone);
                    if (!t.VertEdges[v].empty()) target = w + (sum / (float)t.VertEdges[v].size() - w) * f;
                    break;
                }
                }
                target = std::clamp(target, 0.0f, 1.0f);
                if (std::abs(target - w) > 1e-6f) next.push_back({ v, target });
            }
            for (auto& [v, x] : next) SetWeight(m, v, bone, x);
            changed += next.size();
        }
        return changed;
    }

    glm::vec4 WeightColor(float w)
    {
        // Blender's: blue, cyan, green, yellow, red.
        w = std::clamp(w, 0.0f, 1.0f);
        glm::vec3 stops[5] = { { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } };
        float x = w * 4.0f;
        int i = std::min((int)x, 3);
        return glm::vec4(glm::mix(stops[i], stops[i + 1], x - (float)i), 1.0f);
    }

    // ---- Shape keys

    std::vector<glm::vec3> MixedPositions(const EditableMesh& m)
    {
        EditableMesh c = m.EditedShape >= 0 ? m.Canonical() : EditableMesh();
        const EditableMesh& base = m.EditedShape >= 0 ? c : m;
        std::vector<glm::vec3> out = base.Positions;
        for (const ShapeKey& k : base.ShapeKeys)
            if (k.Value != 0.0f && k.Positions.size() == out.size())
                for (size_t v = 0; v < out.size(); ++v) out[v] += (k.Positions[v] - base.Positions[v]) * k.Value;
        return out;
    }

    int AddShapeKey(EditableMesh& m, const std::string& name, bool fromMix)
    {
        int edited = m.EditedShape;
        SetEditedShape(m, -1);
        ShapeKey k;
        std::string base = name.empty() ? "Key" : name;
        k.Name = base;
        for (int i = 1;; ++i)
        {
            bool taken = false;
            for (const ShapeKey& o : m.ShapeKeys) taken |= o.Name == k.Name;
            if (!taken) break;
            k.Name = base + " " + std::to_string(i);
        }
        k.Positions = fromMix ? MixedPositions(m) : m.Positions;
        m.ShapeKeys.push_back(std::move(k));
        SetEditedShape(m, edited);
        return (int)m.ShapeKeys.size() - 1;
    }

    void RemoveShapeKey(EditableMesh& m, int key)
    {
        if (key < 0 || key >= (int)m.ShapeKeys.size()) return;
        int edited = m.EditedShape;
        SetEditedShape(m, -1);
        m.ShapeKeys.erase(m.ShapeKeys.begin() + key);
        for (Action& a : m.Actions)
            for (ActionKey& k : a.Keys)
            {
                if ((size_t)key < k.Shapes.size()) k.Shapes.erase(k.Shapes.begin() + key);
                if ((size_t)key < k.ShapeCurves.size()) k.ShapeCurves.erase(k.ShapeCurves.begin() + key);
            }
        if (edited == key) edited = -1;
        else if (edited > key) --edited;
        SetEditedShape(m, edited);
    }

    void SetEditedShape(EditableMesh& m, int key)
    {
        if (key >= (int)m.ShapeKeys.size()) key = -1;
        if (key == m.EditedShape) return;
        if (m.EditedShape >= 0 && m.EditedShape < (int)m.ShapeKeys.size()) std::swap(m.Positions, m.ShapeKeys[(size_t)m.EditedShape].Positions);
        m.EditedShape = -1;
        if (key >= 0 && m.ShapeKeys[(size_t)key].Positions.size() == m.Positions.size())
        {
            std::swap(m.Positions, m.ShapeKeys[(size_t)key].Positions);
            m.EditedShape = key;
        }
    }

    // ---- Actions

    int AddAction(EditableMesh& m, const std::string& name)
    {
        Action a;
        std::string base = name.empty() ? "Action" : name;
        a.Name = base;
        for (int i = 1;; ++i)
        {
            bool taken = false;
            for (const Action& o : m.Actions) taken |= o.Name == a.Name;
            if (!taken) break;
            a.Name = base + " " + std::to_string(i);
        }
        m.Actions.push_back(std::move(a));
        return (int)m.Actions.size() - 1;
    }

    void InsertKey(EditableMesh& m, int action, float time, bool selectedOnly, bool shapes)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return;
        time = std::max(time, 0.0f);
        auto& keys = m.Actions[(size_t)action].Keys;
        ActionKey* key = nullptr;
        for (ActionKey& o : keys)
            if (std::abs(o.Time - time) < 1e-4f) key = &o;
        bool created = !key;
        if (created)
        {
            keys.push_back({});
            key = &keys.back();
            key->Time = time;
            key->Keyed.assign(m.Bones.size(), 0);
        }
        size_t n = m.Bones.size();
        key->Rotations.resize(n, glm::quat(1, 0, 0, 0));
        key->Locations.resize(n, glm::vec3(0.0f));
        key->Scales.resize(n, glm::vec3(1.0f));
        if (key->Keyed.empty()) key->Keyed.assign(n, 1); // (an old whole-pose key)
        key->Keyed.resize(n, 0);
        for (size_t b = 0; b < n; ++b)
        {
            if (selectedOnly && !(m.Bones[b].Flags & BoneBody)) continue;
            key->Rotations[b] = m.Bones[b].PoseRotation;
            key->Locations[b] = m.Bones[b].PoseLocation;
            key->Scales[b] = m.Bones[b].PoseScale;
            key->Keyed[b] = 1;
        }
        if (shapes)
        {
            key->Shapes.clear();
            for (const ShapeKey& s : m.ShapeKeys) key->Shapes.push_back(s.Value);
        }
        // Everything keyed: stored as a whole-pose key.
        if (std::all_of(key->Keyed.begin(), key->Keyed.end(), [](uint8_t k) { return k != 0; })) key->Keyed.clear();
        if (created) std::sort(keys.begin(), keys.end(), [](const ActionKey& a, const ActionKey& b) { return a.Time < b.Time; });
    }

    bool MoveKey(EditableMesh& m, int action, float from, float to)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return false;
        auto& keys = m.Actions[(size_t)action].Keys;
        auto it = std::find_if(keys.begin(), keys.end(), [&](const ActionKey& k) { return std::abs(k.Time - from) < 1e-4f; });
        if (it == keys.end()) return false;
        ActionKey moved = std::move(*it);
        keys.erase(it);
        moved.Time = std::max(to, 0.0f);
        auto there = std::find_if(keys.begin(), keys.end(), [&](const ActionKey& k) { return std::abs(k.Time - moved.Time) < 1e-4f; });
        if (there != keys.end())
        {
            // Into the key already there: the moved key's bones (and shapes) win.
            ActionKey& o = *there;
            size_t n = std::max(o.Rotations.size(), moved.Rotations.size());
            if (o.Keyed.empty()) o.Keyed.assign(o.Rotations.size(), 1);
            o.Rotations.resize(n, glm::quat(1, 0, 0, 0));
            o.Locations.resize(n, glm::vec3(0.0f));
            o.Scales.resize(n, glm::vec3(1.0f));
            o.Keyed.resize(n, 0);
            for (size_t b = 0; b < n; ++b)
                if (moved.HasBone(b))
                {
                    o.Rotations[b] = moved.Rotations[b];
                    o.Locations[b] = moved.Locations[b];
                    o.Scales[b] = moved.Scales[b];
                    o.Keyed[b] = 1;
                    if (b < moved.Curves.size()) o.EditCurve(b) = moved.Curves[b];
                }
            if (!moved.Shapes.empty())
            {
                o.Shapes = moved.Shapes;
                o.ShapeCurves = moved.ShapeCurves;
            }
            if (std::all_of(o.Keyed.begin(), o.Keyed.end(), [](uint8_t k) { return k != 0; })) o.Keyed.clear();
        }
        else
            keys.push_back(std::move(moved));
        std::sort(keys.begin(), keys.end(), [](const ActionKey& a, const ActionKey& b) { return a.Time < b.Time; });
        return true;
    }

    void DeleteKey(EditableMesh& m, int action, float time)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return;
        auto& keys = m.Actions[(size_t)action].Keys;
        keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const ActionKey& k) { return std::abs(k.Time - time) < 1e-4f; }), keys.end());
    }

    namespace
    {
        // The bones' poses in `action` at `time` (per bone, between the keys around it that hold it).
        void PoseAt(const Action& action, float time, std::vector<Bone>& bones)
        {
            const auto& keys = action.Keys;
            if (keys.empty()) return;
            for (size_t b = 0; b < bones.size(); ++b)
            {
                Bone& bone = bones[b];
                BonePoseAt(action, b, time, bone.PoseLocation, bone.PoseRotation, bone.PoseScale);
            }
        }
    }

    void EvaluateAction(EditableMesh& m, int action, float time)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return;
        const auto& keys = m.Actions[(size_t)action].Keys;
        if (keys.empty()) return;
        PoseAt(m.Actions[(size_t)action], time, m.Bones);
        // Shape key values, along their curves between the keys that hold them.
        for (size_t s = 0; s < m.ShapeKeys.size(); ++s)
        {
            float v;
            if (ShapeValue(m.Actions[(size_t)action], s, time, v)) m.ShapeKeys[s].Value = v;
        }
    }

    AnimationClip ToClip(const EditableMesh& m, const Action& action)
    {
        AnimationClip clip;
        clip.Name = action.Name;
        clip.Duration = action.Duration();
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            AnimationChannel t, r, s;
            t.Joint = r.Joint = s.Joint = (int)b;
            t.Path = AnimationPath::Translation;
            r.Path = AnimationPath::Rotation;
            s.Path = AnimationPath::Scale;
            glm::mat4 rest = RestLocal(m, (int)b);
            // Curves the clip's linear keys can't follow (Bezier, Constant): sampled at the keys and 30 times a second.
            bool curved = false;
            for (const ActionKey& k : action.Keys) curved |= k.HasBone(b) && k.CurveOf(b).Interp != KeyInterp::Linear;
            if (curved)
            {
                BoneTrack track = TrackOf(action, b);
                std::vector<float> times;
                for (float x = track.Times.front(); x < track.Times.back(); x += 1.0f / 30.0f) times.push_back(x);
                times.insert(times.end(), track.Times.begin(), track.Times.end());
                // (just before each key after a Constant stretch, so the step stays a step)
                for (size_t i = 0; i + 1 < track.Keys.size(); ++i)
                    if (action.Keys[track.Keys[i]].CurveOf(b).Interp == KeyInterp::Constant) times.push_back(std::max(track.Times[i], track.Times[i + 1] - 1e-3f));
                std::sort(times.begin(), times.end());
                times.erase(std::unique(times.begin(), times.end(), [](float x, float y) { return std::abs(x - y) < 1e-5f; }), times.end());
                for (float x : times)
                {
                    Bone posed = m.Bones[b];
                    BonePoseAt(action, b, x, posed.PoseLocation, posed.PoseRotation, posed.PoseScale);
                    glm::vec3 tr, sc;
                    glm::quat rot;
                    Decompose(rest * PoseDelta(posed), tr, rot, sc);
                    for (AnimationChannel* c : { &t, &r, &s }) c->Times.push_back(x);
                    t.Values.push_back(glm::vec4(tr, 0.0f));
                    r.Values.push_back(glm::vec4(rot.x, rot.y, rot.z, rot.w));
                    s.Values.push_back(glm::vec4(sc, 0.0f));
                }
                clip.Channels.push_back(std::move(t));
                clip.Channels.push_back(std::move(r));
                clip.Channels.push_back(std::move(s));
                continue;
            }
            for (const ActionKey& k : action.Keys)
            {
                if (!k.HasBone(b) && !k.Keyed.empty()) continue; // (keys without this bone: no key for it here)
                Bone posed = m.Bones[b];
                if (b < k.Rotations.size())
                {
                    posed.PoseRotation = k.Rotations[b];
                    posed.PoseLocation = k.Locations[b];
                    posed.PoseScale = k.Scales[b];
                }
                else
                {
                    posed.PoseRotation = glm::quat(1, 0, 0, 0);
                    posed.PoseLocation = glm::vec3(0.0f);
                    posed.PoseScale = glm::vec3(1.0f);
                }
                glm::vec3 tr, sc;
                glm::quat rot;
                Decompose(rest * PoseDelta(posed), tr, rot, sc);
                for (AnimationChannel* c : { &t, &r, &s }) c->Times.push_back(k.Time);
                t.Values.push_back(glm::vec4(tr, 0.0f));
                r.Values.push_back(glm::vec4(rot.x, rot.y, rot.z, rot.w));
                s.Values.push_back(glm::vec4(sc, 0.0f));
            }
            if (t.Times.empty()) continue;
            clip.Channels.push_back(std::move(t));
            clip.Channels.push_back(std::move(r));
            clip.Channels.push_back(std::move(s));
        }
        // Keyed shape key values: weight channels (blend shape index, value in x).
        for (size_t sk = 0; sk < m.ShapeKeys.size(); ++sk)
        {
            AnimationChannel w;
            w.Joint = (int)sk;
            w.Path = AnimationPath::Translation;
            bool curved = false;
            for (const ActionKey& k : action.Keys) curved |= k.HasShape(sk) && k.ShapeCurveOf(sk).Interp != KeyInterp::Linear;
            if (curved)
            {
                // (sampled at the keys and 30 times a second between, just before keys after a Constant stretch)
                BoneTrack track = ShapeTrackOf(action, sk);
                std::vector<float> times(track.Times.begin(), track.Times.end());
                for (float x = track.Times.front(); x < track.Times.back(); x += 1.0f / 30.0f) times.push_back(x);
                for (size_t i = 0; i + 1 < track.Keys.size(); ++i)
                    if (action.Keys[track.Keys[i]].ShapeCurveOf(sk).Interp == KeyInterp::Constant)
                        times.push_back(std::max(track.Times[i], track.Times[i + 1] - 1e-3f));
                std::sort(times.begin(), times.end());
                times.erase(std::unique(times.begin(), times.end(), [](float x, float y) { return std::abs(x - y) < 1e-5f; }), times.end());
                for (float x : times)
                {
                    float v = 0.0f;
                    ShapeValue(action, sk, x, v);
                    w.Times.push_back(x);
                    w.Values.push_back(glm::vec4(v, 0.0f, 0.0f, 0.0f));
                }
            }
            else
                for (const ActionKey& k : action.Keys)
                    if (sk < k.Shapes.size())
                    {
                        w.Times.push_back(k.Time);
                        w.Values.push_back(glm::vec4(k.Shapes[sk], 0.0f, 0.0f, 0.0f));
                    }
            if (!w.Times.empty()) clip.WeightChannels.push_back(std::move(w));
        }
        // B-bone segments bend with the bones around them: their transforms (relative to their bone) sampled at the
        // keys and 30 times a second between.
        SkinLayout layout = MakeSkinLayout(m);
        if (layout.JointCount > m.Bones.size() && !action.Keys.empty())
        {
            std::vector<float> times;
            float first = action.Keys.front().Time, last = action.Keys.back().Time;
            for (float t = first; t < last; t += 1.0f / 30.0f) times.push_back(t);
            for (const ActionKey& k : action.Keys) times.push_back(k.Time);
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end(), [](float a, float b) { return std::abs(a - b) < 1e-4f; }), times.end());
            std::vector<AnimationChannel> seg(3 * (layout.JointCount - m.Bones.size()));
            for (size_t j = 0; j < seg.size(); ++j)
            {
                seg[j].Joint = (int)(m.Bones.size() + j / 3);
                seg[j].Path = j % 3 == 0 ? AnimationPath::Translation : j % 3 == 1 ? AnimationPath::Rotation : AnimationPath::Scale;
            }
            EditableMesh posed;
            posed.Bones = m.Bones;
            for (float t : times)
            {
                PoseAt(action, t, posed.Bones);
                std::vector<glm::mat4> frames = PoseMatrices(posed);
                for (size_t b = 0; b < m.Bones.size(); ++b)
                {
                    if (layout.First[b] < 0) continue;
                    std::vector<glm::mat4> segs = BBoneSegments(posed, (int)b, frames);
                    glm::mat4 inv = glm::inverse(frames[b]);
                    for (size_t k = 0; k < segs.size(); ++k)
                    {
                        glm::vec3 tr, sc;
                        glm::quat rot;
                        Decompose(inv * segs[k], tr, rot, sc);
                        size_t base = ((size_t)layout.First[b] - m.Bones.size() + k) * 3;
                        for (int c = 0; c < 3; ++c) seg[base + (size_t)c].Times.push_back(t);
                        seg[base].Values.push_back(glm::vec4(tr, 0.0f));
                        seg[base + 1].Values.push_back(glm::vec4(rot.x, rot.y, rot.z, rot.w));
                        seg[base + 2].Values.push_back(glm::vec4(sc, 0.0f));
                    }
                }
            }
            for (AnimationChannel& c : seg) clip.Channels.push_back(std::move(c));
        }
        return clip;
    }

    // ---- Graph Editor

    const char* ChannelName(int channel)
    {
        static const char* names[] = { "Location X", "Location Y", "Location Z", "Rotation X", "Rotation Y", "Rotation Z", "Scale X", "Scale Y", "Scale Z" };
        return names[std::clamp(channel, 0, kBoneChannels - 1)];
    }

    glm::quat EulerToQuat(const glm::vec3& e)
    {
        return glm::angleAxis(e.z, glm::vec3(0, 0, 1)) * glm::angleAxis(e.y, glm::vec3(0, 1, 0)) * glm::angleAxis(e.x, glm::vec3(1, 0, 0));
    }

    glm::vec3 QuatToEuler(const glm::quat& q)
    {
        // R = Rz Ry Rx (glm: m[column][row]).
        glm::mat3 r = glm::mat3_cast(glm::normalize(q));
        float sy = std::clamp(-r[0][2], -1.0f, 1.0f);
        float y = std::asin(sy);
        if (std::abs(sy) < 0.99999f) return glm::vec3(std::atan2(r[1][2], r[2][2]), y, std::atan2(r[0][1], r[0][0]));
        // Gimbal lock: X and Z turn about the same axis; all of it on X.
        return glm::vec3(std::atan2(-r[2][1], r[1][1]), y, 0.0f);
    }

    namespace
    {
        // Of the Euler angles giving a turn (the other solution x+pi, pi-y, z+pi, and whole turns), the ones nearest `near`.
        glm::vec3 NearestEuler(const glm::quat& q, const glm::vec3& near)
        {
            glm::vec3 e = QuatToEuler(q);
            auto wrap = [](float a, float to) {
                float twoPi = glm::two_pi<float>();
                return a + twoPi * std::round((to - a) / twoPi);
            };
            glm::vec3 alt(e.x + glm::pi<float>(), glm::pi<float>() - e.y, e.z + glm::pi<float>());
            glm::vec3 c1(wrap(e.x, near.x), wrap(e.y, near.y), wrap(e.z, near.z));
            glm::vec3 c2(wrap(alt.x, near.x), wrap(alt.y, near.y), wrap(alt.z, near.z));
            return glm::length(c1 - near) <= glm::length(c2 - near) ? c1 : c2;
        }

        // The Euler angles along a slerp from `a` (whose angles are `ea`) towards `b`, `s` of the way: followed in small
        // steps so they change continuously with the turn.
        glm::vec3 FollowSlerp(const glm::quat& a, glm::vec3 ea, const glm::quat& b, float s)
        {
            int steps = std::max(1, (int)std::ceil(24.0f * std::abs(s)));
            for (int k = 1; k <= steps; ++k) ea = NearestEuler(glm::slerp(a, b, s * (float)k / (float)steps), ea);
            return ea;
        }

        std::array<float, kBoneChannels> Channels(const glm::vec3& loc, const glm::vec3& euler, const glm::vec3& scale)
        {
            std::array<float, kBoneChannels> v{};
            for (int c = 0; c < 3; ++c)
            {
                v[(size_t)c] = loc[c];
                v[3 + (size_t)c] = euler[c];
                v[6 + (size_t)c] = scale[c];
            }
            return v;
        }

        float Hermite(float v0, float m0, float v1, float m1, float h, float s)
        {
            float s2 = s * s, s3 = s2 * s;
            return (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * h * m0 + (-2 * s3 + 3 * s2) * v1 + (s3 - s2) * h * m1;
        }
    }

    BoneTrack TrackOf(const Action& action, size_t bone)
    {
        // Each key's Euler angles continue from the key before: along the slerp a linear stretch makes, else the
        // nearest to the last key's.
        BoneTrack t;
        glm::vec3 last(0.0f);
        const ActionKey* prev = nullptr;
        for (size_t k = 0; k < action.Keys.size(); ++k)
        {
            const ActionKey& key = action.Keys[k];
            if (!key.HasBone(bone)) continue;
            glm::vec3 e = !prev ? QuatToEuler(key.Rotations[bone])
                          : prev->CurveOf(bone).Interp == KeyInterp::Linear ? FollowSlerp(prev->Rotations[bone], last, key.Rotations[bone], 1.0f)
                                                                            : NearestEuler(key.Rotations[bone], last);
            last = e;
            prev = &key;
            t.Keys.push_back(k);
            t.Times.push_back(key.Time);
            t.Values.push_back(Channels(key.Locations[bone], e, key.Scales[bone]));
        }
        return t;
    }

    namespace
    {
        // A track key's slopes for one channel: its free handles, or automatic clamped ones.
        void Slopes(const KeyCurve& kc, const BoneTrack& t, size_t i, int c, float& in, float& out)
        {
            if (kc.Free & (1u << c))
            {
                in = kc.SlopeIn[(size_t)c];
                out = kc.SlopeOut[(size_t)c];
                return;
            }
            in = out = 0.0f;
            size_t n = t.Keys.size();
            if (i == 0 || i + 1 >= n) return;
            float a = t.Values[i - 1][(size_t)c], v = t.Values[i][(size_t)c], b = t.Values[i + 1][(size_t)c];
            if ((v >= a && v >= b) || (v <= a && v <= b)) return; // (a peak or dip stays level: no overshoot)
            float span = t.Times[i + 1] - t.Times[i - 1];
            if (span > 1e-6f) in = out = (b - a) / span;
        }
    }

    void ShapeKeySlopes(const Action& action, const BoneTrack& t, size_t shape, size_t i, float& in, float& out)
    {
        Slopes(action.Keys[t.Keys[i]].ShapeCurveOf(shape), t, i, 0, in, out);
    }

    BoneTrack ShapeTrackOf(const Action& action, size_t shape)
    {
        BoneTrack t;
        for (size_t k = 0; k < action.Keys.size(); ++k)
        {
            const ActionKey& key = action.Keys[k];
            if (!key.HasShape(shape)) continue;
            std::array<float, kBoneChannels> v{};
            v[0] = key.Shapes[shape];
            t.Keys.push_back(k);
            t.Times.push_back(key.Time);
            t.Values.push_back(v);
        }
        return t;
    }

    bool ShapeValue(const Action& action, size_t shape, float time, float& value)
    {
        BoneTrack t = ShapeTrackOf(action, shape);
        if (t.Keys.empty()) return false;
        size_t i = 0;
        while (i + 1 < t.Times.size() && t.Times[i + 1] <= time) ++i;
        value = t.Values[i][0];
        if (i + 1 >= t.Times.size() || time <= t.Times[i]) return true;
        float span = t.Times[i + 1] - t.Times[i];
        float s = span > 1e-6f ? std::clamp((time - t.Times[i]) / span, 0.0f, 1.0f) : 0.0f;
        switch (action.Keys[t.Keys[i]].ShapeCurveOf(shape).Interp)
        {
        case KeyInterp::Constant: break;
        case KeyInterp::Linear: value = glm::mix(t.Values[i][0], t.Values[i + 1][0], s); break;
        case KeyInterp::Bezier:
        {
            float in0, out0, in1, out1;
            ShapeKeySlopes(action, t, shape, i, in0, out0);
            ShapeKeySlopes(action, t, shape, i + 1, in1, out1);
            value = Hermite(t.Values[i][0], out0, t.Values[i + 1][0], in1, span, s);
            break;
        }
        }
        return true;
    }

    namespace
    {
        bool KeyHoldsShape(const EditableMesh& m, int action, size_t key, size_t shape)
        {
            return action >= 0 && action < (int)m.Actions.size() && key < m.Actions[(size_t)action].Keys.size() &&
                   m.Actions[(size_t)action].Keys[key].HasShape(shape);
        }
    }

    void SetShapeValue(EditableMesh& m, int action, size_t key, size_t shape, float value)
    {
        if (KeyHoldsShape(m, action, key, shape)) m.Actions[(size_t)action].Keys[key].Shapes[shape] = value;
    }

    void SetShapeInterpolation(EditableMesh& m, int action, size_t key, size_t shape, KeyInterp interp)
    {
        if (KeyHoldsShape(m, action, key, shape)) m.Actions[(size_t)action].Keys[key].EditShapeCurve(shape).Interp = interp;
    }

    void SetShapeHandles(EditableMesh& m, int action, size_t key, size_t shape, float slopeIn, float slopeOut)
    {
        if (!KeyHoldsShape(m, action, key, shape)) return;
        KeyCurve& c = m.Actions[(size_t)action].Keys[key].EditShapeCurve(shape);
        c.Free |= 1;
        c.SlopeIn[0] = slopeIn;
        c.SlopeOut[0] = slopeOut;
    }

    void SetShapeAutoHandles(EditableMesh& m, int action, size_t key, size_t shape)
    {
        if (!KeyHoldsShape(m, action, key, shape)) return;
        KeyCurve& c = m.Actions[(size_t)action].Keys[key].EditShapeCurve(shape);
        c.Free &= (uint16_t)0xFFFEu; // (bit 0 off)
        c.SlopeIn[0] = c.SlopeOut[0] = 0.0f;
    }

    void KeySlopes(const Action& action, const BoneTrack& t, size_t bone, size_t i, int c, float& in, float& out)
    {
        Slopes(action.Keys[t.Keys[i]].CurveOf(bone), t, i, c, in, out);
    }

    void BonePoseAt(const Action& action, size_t b, float time, glm::vec3& location, glm::quat& rotation, glm::vec3& scale)
    {
        const auto& keys = action.Keys;
        int lo = -1, hi = -1;
        for (int k = 0; k < (int)keys.size(); ++k)
        {
            if (!keys[(size_t)k].HasBone(b)) continue;
            if (keys[(size_t)k].Time <= time) lo = k;
            else if (hi < 0) hi = k;
        }
        if (lo < 0 && hi < 0)
        {
            rotation = glm::quat(1, 0, 0, 0);
            location = glm::vec3(0.0f);
            scale = glm::vec3(1.0f);
            return;
        }
        if (lo < 0) lo = hi;
        if (hi < 0) hi = lo;
        const ActionKey &a = keys[(size_t)lo], &c = keys[(size_t)hi];
        float span = c.Time - a.Time;
        float t = span > 1e-6f ? std::clamp((time - a.Time) / span, 0.0f, 1.0f) : 0.0f;
        KeyInterp interp = lo == hi ? KeyInterp::Constant : a.CurveOf(b).Interp;
        if (interp == KeyInterp::Constant)
        {
            const ActionKey& k = t >= 1.0f ? c : a;
            rotation = k.Rotations[b];
            location = k.Locations[b];
            scale = k.Scales[b];
            return;
        }
        if (interp == KeyInterp::Linear)
        {
            rotation = glm::slerp(a.Rotations[b], c.Rotations[b], t);
            location = glm::mix(a.Locations[b], c.Locations[b], t);
            scale = glm::mix(a.Scales[b], c.Scales[b], t);
            return;
        }
        // Bezier: each channel along its curve (cubic Hermite with the keys' slopes); rotation through Euler angles.
        BoneTrack track = TrackOf(action, b);
        size_t i = 0;
        while (i + 1 < track.Keys.size() && (int)track.Keys[i] != lo) ++i;
        std::array<float, kBoneChannels> v{};
        for (int ch = 0; ch < kBoneChannels; ++ch)
        {
            float in0, out0, in1, out1;
            KeySlopes(action, track, b, i, ch, in0, out0);
            KeySlopes(action, track, b, i + 1, ch, in1, out1);
            v[(size_t)ch] = Hermite(track.Values[i][(size_t)ch], out0, track.Values[i + 1][(size_t)ch], in1, span, t);
        }
        location = glm::vec3(v[0], v[1], v[2]);
        rotation = EulerToQuat(glm::vec3(v[3], v[4], v[5]));
        scale = glm::vec3(v[6], v[7], v[8]);
    }

    float ChannelValue(const Action& action, size_t bone, int channel, float time)
    {
        glm::vec3 loc, scl;
        glm::quat rot;
        BonePoseAt(action, bone, time, loc, rot, scl);
        if (channel < 3) return loc[channel];
        if (channel >= 6) return scl[channel - 6];
        // Rotation: continuous with the track. Linear stretches follow their slerp from the key before; Bezier ones are
        // Euler curves already (the turn's angles nearest them).
        BoneTrack track = TrackOf(action, bone);
        if (track.Times.empty()) return 0.0f;
        size_t i = 0;
        while (i + 1 < track.Times.size() && track.Times[i + 1] <= time) ++i;
        glm::vec3 ei(track.Values[i][3], track.Values[i][4], track.Values[i][5]);
        if (i + 1 >= track.Times.size() || time <= track.Times[i]) return ei[channel - 3];
        const ActionKey &a = action.Keys[track.Keys[i]], &b = action.Keys[track.Keys[i + 1]];
        float span = track.Times[i + 1] - track.Times[i];
        float s = span > 1e-6f ? std::clamp((time - track.Times[i]) / span, 0.0f, 1.0f) : 0.0f;
        KeyInterp interp = a.CurveOf(bone).Interp;
        if (interp == KeyInterp::Constant) return ei[channel - 3];
        if (interp == KeyInterp::Linear) return FollowSlerp(a.Rotations[bone], ei, b.Rotations[bone], s)[channel - 3];
        // (the curve itself: no round trip through the turn, which loses the angles at gimbal lock)
        float in0, out0, in1, out1;
        KeySlopes(action, track, bone, i, channel, in0, out0);
        KeySlopes(action, track, bone, i + 1, channel, in1, out1);
        return Hermite(track.Values[i][(size_t)channel], out0, track.Values[i + 1][(size_t)channel], in1, span, s);
    }

    namespace
    {
        bool KeyHolds(const EditableMesh& m, int action, size_t key, size_t bone)
        {
            return action >= 0 && action < (int)m.Actions.size() && key < m.Actions[(size_t)action].Keys.size() &&
                   m.Actions[(size_t)action].Keys[key].HasBone(bone);
        }
    }

    void SetChannelValue(EditableMesh& m, int action, size_t key, size_t bone, int channel, float value)
    {
        if (!KeyHolds(m, action, key, bone) || channel < 0 || channel >= kBoneChannels) return;
        Action& a = m.Actions[(size_t)action];
        ActionKey& k = a.Keys[key];
        if (channel < 3) k.Locations[bone][channel] = value;
        else if (channel >= 6) k.Scales[bone][channel - 6] = value;
        else
        {
            // The key's Euler angles as the graph shows them (continuous with the keys before), one changed.
            BoneTrack track = TrackOf(a, bone);
            size_t i = 0;
            while (i < track.Keys.size() && track.Keys[i] != key) ++i;
            glm::vec3 e = i < track.Keys.size() ? glm::vec3(track.Values[i][3], track.Values[i][4], track.Values[i][5]) : QuatToEuler(k.Rotations[bone]);
            e[channel - 3] = value;
            k.Rotations[bone] = EulerToQuat(e);
        }
    }

    void SetInterpolation(EditableMesh& m, int action, size_t key, size_t bone, KeyInterp interp)
    {
        if (!KeyHolds(m, action, key, bone)) return;
        m.Actions[(size_t)action].Keys[key].EditCurve(bone).Interp = interp;
    }

    void SetHandles(EditableMesh& m, int action, size_t key, size_t bone, int channel, float slopeIn, float slopeOut)
    {
        if (!KeyHolds(m, action, key, bone) || channel < 0 || channel >= kBoneChannels) return;
        KeyCurve& c = m.Actions[(size_t)action].Keys[key].EditCurve(bone);
        c.Free |= (uint16_t)(1u << channel);
        c.SlopeIn[(size_t)channel] = slopeIn;
        c.SlopeOut[(size_t)channel] = slopeOut;
    }

    void SetAutoHandles(EditableMesh& m, int action, size_t key, size_t bone, int channel)
    {
        if (!KeyHolds(m, action, key, bone) || channel < 0 || channel >= kBoneChannels) return;
        KeyCurve& c = m.Actions[(size_t)action].Keys[key].EditCurve(bone);
        c.Free &= (uint16_t)~(1u << channel);
        c.SlopeIn[(size_t)channel] = c.SlopeOut[(size_t)channel] = 0.0f;
    }

    // ---- IK

    float SolveIK(EditableMesh& m, int bone, const glm::vec3& target, int chain, int iterations)
    {
        if (bone < 0 || bone >= (int)m.Bones.size()) return 0.0f;
        std::vector<int> joints;
        for (int b = bone; b >= 0 && b < (int)m.Bones.size() && (int)joints.size() < std::max(chain, 1); b = m.Bones[(size_t)b].Parent) joints.push_back(b);
        float len = glm::length(m.Bones[(size_t)bone].Tail - m.Bones[(size_t)bone].Head);
        auto tip = [&](const std::vector<glm::mat4>& pose) { return glm::vec3(pose[(size_t)bone] * glm::vec4(0, len, 0, 1)); };
        std::vector<glm::mat4> pose = PoseMatrices(m);
        // Turns joint j (in its own frame) by the shortest turn taking direction `from` onto `to` (model space).
        auto turnJoint = [&](int j, glm::vec3 from, glm::vec3 to) {
            if (glm::length(from) < 1e-9f || glm::length(to) < 1e-9f) return;
            from = glm::normalize(from);
            to = glm::normalize(to);
            float c = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
            if (c > 0.999999f) return;
            glm::vec3 axis = glm::cross(from, to);
            if (glm::length(axis) < 1e-9f) return;
            glm::quat turn = glm::angleAxis(std::acos(c), glm::normalize(axis));
            const Bone& bj = m.Bones[(size_t)j];
            glm::mat4 frame = (bj.Parent >= 0 && bj.Parent < (int)pose.size() ? pose[(size_t)bj.Parent] : glm::mat4(1.0f)) * RestLocal(m, j);
            glm::mat3 r(glm::normalize(glm::vec3(frame[0])), glm::normalize(glm::vec3(frame[1])), glm::normalize(glm::vec3(frame[2])));
            glm::quat fr = glm::normalize(glm::quat_cast(r));
            m.Bones[(size_t)j].PoseRotation = glm::normalize(glm::inverse(fr) * turn * fr * bj.PoseRotation);
            pose = PoseMatrices(m);
        };
        if (joints.size() == 2)
        {
            // Two bones: exactly (law of cosines), bending the way the chain already bends.
            int upper = joints[1], lower = joints[0];
            glm::vec3 a = glm::vec3(pose[(size_t)upper][3]), b = glm::vec3(pose[(size_t)lower][3]), c = tip(pose);
            float l1 = glm::length(b - a), l2 = glm::length(c - b);
            glm::vec3 toT = target - a;
            float d = glm::length(toT);
            if (l1 > 1e-9f && l2 > 1e-9f && d > 1e-9f)
            {
                d = std::clamp(d, std::abs(l1 - l2) + 1e-5f, l1 + l2 - 1e-5f);
                glm::vec3 dir = glm::normalize(toT);
                glm::vec3 bend = (b - a) - dir * glm::dot(b - a, dir); // the elbow's side
                if (glm::length(bend) < 1e-6f) bend = (c - a) - dir * glm::dot(c - a, dir);
                if (glm::length(bend) < 1e-6f) bend = glm::cross(dir, std::abs(dir.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
                bend = glm::normalize(bend);
                float cosA = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
                glm::vec3 elbow = a + (dir * cosA + bend * std::sqrt(1.0f - cosA * cosA)) * l1;
                turnJoint(upper, b - a, elbow - a);
                glm::vec3 nb = glm::vec3(pose[(size_t)lower][3]);
                turnJoint(lower, tip(pose) - nb, target - nb);
            }
            return glm::length(tip(pose) - target);
        }
        for (int it = 0; it < iterations; ++it)
        {
            for (int j : joints)
            {
                glm::vec3 head = glm::vec3(pose[(size_t)j][3]);
                glm::vec3 a = tip(pose) - head, b = target - head;
                if (glm::length(a) < 1e-9f || glm::length(b) < 1e-9f) continue;
                a = glm::normalize(a);
                b = glm::normalize(b);
                float c = std::clamp(glm::dot(a, b), -1.0f, 1.0f);
                if (c > 0.99999f) continue;
                glm::vec3 axis = glm::cross(a, b);
                if (glm::length(axis) < 1e-9f) continue;
                glm::quat turn = glm::angleAxis(std::acos(c), glm::normalize(axis)); // (model space)
                // Into the bone's own frame (the one its pose rotation acts in).
                const Bone& bj = m.Bones[(size_t)j];
                glm::mat4 frame = (bj.Parent >= 0 && bj.Parent < (int)pose.size() ? pose[(size_t)bj.Parent] : glm::mat4(1.0f)) * RestLocal(m, j);
                glm::mat3 r(glm::normalize(glm::vec3(frame[0])), glm::normalize(glm::vec3(frame[1])), glm::normalize(glm::vec3(frame[2])));
                glm::quat fr = glm::normalize(glm::quat_cast(r));
                m.Bones[(size_t)j].PoseRotation = glm::normalize(glm::inverse(fr) * turn * fr * bj.PoseRotation);
                pose = PoseMatrices(m);
            }
            if (glm::length(tip(pose) - target) < 1e-5f) break;
        }
        return glm::length(tip(pose) - target);
    }

    void ApplyConstraints(EditableMesh& m)
    {
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            int target = m.Bones[b].IKTarget;
            if (target < 0 || target >= (int)m.Bones.size() || target == (int)b) continue;
            glm::vec3 goal = glm::vec3(PoseMatrices(m)[(size_t)target][3]);
            SolveIK(m, (int)b, goal, m.Bones[b].IKChain);
        }
    }
}
