#include "Dimension/Modeling/Rig.h"
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
        std::vector<glm::mat4> pose = PoseMatrices(m);
        for (size_t b = 0; b < pose.size(); ++b) pose[b] = pose[b] * glm::inverse(RestMatrix(m.Bones[b]));
        return pose;
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
        m.Bones.push_back(b);
        int index = (int)m.Bones.size() - 1;
        for (Action& a : m.Actions)
            for (ActionKey& k : a.Keys)
                if (k.Rotations.size() == (size_t)index)
                {
                    k.Rotations.push_back(glm::quat(1, 0, 0, 0));
                    k.Locations.push_back(glm::vec3(0.0f));
                    k.Scales.push_back(glm::vec3(1.0f));
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
                for (size_t b = 0; b < n && b < k.Rotations.size(); ++b)
                    if (!gone[b])
                    {
                        out.Rotations.push_back(k.Rotations[b]);
                        out.Locations.push_back(k.Locations[b]);
                        out.Scales.push_back(k.Scales[b]);
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
        // Initial weights: the bones each vertex sees, by inverse distance (to the 4th power: the nearest dominates).
        std::vector<std::vector<VertexWeight>> w(n);
        for (uint32_t v = 0; v < n; ++v)
        {
            if (!chosen[v]) continue;
            glm::vec3 p = m.Positions[v], inside = p - normals[v] * eps;
            int nearest = -1;
            float nearestD = 1e30f;
            for (int b : deform)
            {
                glm::vec3 q = ClosestOnSegment(p, m.Bones[(size_t)b].Head, m.Bones[(size_t)b].Tail);
                float d = std::max(glm::length(p - q), eps);
                if (d < nearestD)
                {
                    nearestD = d;
                    nearest = b;
                }
                // Hidden behind a wall of the mesh (another part of the body)? Seen when any of a few points along the bone
                // is (the nearest one can sit on the surface where a bone pokes out).
                const Bone& bone = m.Bones[(size_t)b];
                bool seen = false;
                for (float k : { -1.0f, 0.0f, 0.25f, 0.5f, 0.75f })
                    if (!seen) seen = !rays.Occluded(inside, k < 0.0f ? q : glm::mix(bone.Head, bone.Tail, k));
                if (!seen) continue;
                w[v].push_back({ (uint16_t)b, 1.0f / (d * d * d * d) });
            }
            if (w[v].empty() && nearest >= 0) w[v].push_back({ (uint16_t)nearest, 1.0f });
            Normalize(w[v]);
        }
        // Smoothing along edges (heat spreading out from the bones).
        const Topology& t = m.Topo();
        for (int it = 0; it < 4; ++it)
        {
            std::vector<std::vector<VertexWeight>> next = w;
            for (uint32_t v = 0; v < n; ++v)
            {
                if (!chosen[v] || v >= t.VertEdges.size() || t.VertEdges[v].empty()) continue; // (vertices added since: no edges yet)
                std::unordered_map<uint16_t, float> sum;
                for (const VertexWeight& x : w[v]) sum[x.Bone] += x.Weight * 0.5f;
                float k = 0.5f / (float)t.VertEdges[v].size();
                for (uint32_t e : t.VertEdges[v])
                {
                    uint32_t u = EdgeA(t.Edges[e]) == v ? EdgeB(t.Edges[e]) : EdgeA(t.Edges[e]);
                    const auto& src = chosen[u] ? w[u] : m.Weights[u];
                    for (const VertexWeight& x : src) sum[x.Bone] += x.Weight * k;
                }
                next[v].clear();
                for (auto& [b, x] : sum) next[v].push_back({ b, x });
                Normalize(next[v]);
            }
            w = std::move(next);
        }
        for (uint32_t v = 0; v < n; ++v)
            if (chosen[v]) m.Weights[v] = w[v];
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

    void InsertKey(EditableMesh& m, int action, float time)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return;
        ActionKey k;
        k.Time = std::max(time, 0.0f);
        for (const Bone& b : m.Bones)
        {
            k.Rotations.push_back(b.PoseRotation);
            k.Locations.push_back(b.PoseLocation);
            k.Scales.push_back(b.PoseScale);
        }
        auto& keys = m.Actions[(size_t)action].Keys;
        for (ActionKey& o : keys)
            if (std::abs(o.Time - k.Time) < 1e-4f)
            {
                o = std::move(k);
                return;
            }
        keys.push_back(std::move(k));
        std::sort(keys.begin(), keys.end(), [](const ActionKey& a, const ActionKey& b) { return a.Time < b.Time; });
    }

    void DeleteKey(EditableMesh& m, int action, float time)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return;
        auto& keys = m.Actions[(size_t)action].Keys;
        keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const ActionKey& k) { return std::abs(k.Time - time) < 1e-4f; }), keys.end());
    }

    void EvaluateAction(EditableMesh& m, int action, float time)
    {
        if (action < 0 || action >= (int)m.Actions.size()) return;
        const auto& keys = m.Actions[(size_t)action].Keys;
        if (keys.empty()) return;
        size_t hi = 0;
        while (hi < keys.size() && keys[hi].Time < time) ++hi;
        size_t lo = hi == 0 ? 0 : hi - 1;
        hi = std::min(hi, keys.size() - 1);
        float span = keys[hi].Time - keys[lo].Time;
        float t = span > 1e-6f ? std::clamp((time - keys[lo].Time) / span, 0.0f, 1.0f) : 0.0f;
        for (size_t b = 0; b < m.Bones.size(); ++b)
        {
            Bone& bone = m.Bones[b];
            if (b >= keys[lo].Rotations.size() || b >= keys[hi].Rotations.size())
            {
                bone.PoseRotation = glm::quat(1, 0, 0, 0);
                bone.PoseLocation = glm::vec3(0.0f);
                bone.PoseScale = glm::vec3(1.0f);
                continue;
            }
            bone.PoseRotation = glm::slerp(keys[lo].Rotations[b], keys[hi].Rotations[b], t);
            bone.PoseLocation = glm::mix(keys[lo].Locations[b], keys[hi].Locations[b], t);
            bone.PoseScale = glm::mix(keys[lo].Scales[b], keys[hi].Scales[b], t);
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
            for (const ActionKey& k : action.Keys)
            {
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
        return clip;
    }
}
