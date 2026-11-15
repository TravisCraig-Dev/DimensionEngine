#include "Dimension/Modeling/Modifiers.h"
#include "Dimension/Modeling/MeshOps.h"
#include "Dimension/Modeling/MeshTools.h"
#include "Dimension/Renderer/MeshSimplify.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace Dimension::Modeling
{
    using json = nlohmann::json;

    // ---- The record

    const char* ModifierTypeName(ModifierType type)
    {
        static const char* names[] = { "Subdivision Surface", "Mirror", "Array", "Bevel", "Solidify", "Boolean", "Decimate", "Weld", "Displace" };
        int i = (int)type;
        return i >= 0 && i < (int)ModifierType::Count ? names[i] : "Unknown";
    }

    namespace
    {
        const char* kTypeKeys[] = { "subdivision", "mirror", "array", "bevel", "solidify", "boolean", "decimate", "weld", "displace" };
        json Vec(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }
        glm::vec3 ReadVec(const json& j, const char* key, glm::vec3 fallback)
        {
            if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return fallback;
            return { j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>() };
        }
    }

    Modifier Modifier::Make(ModifierType type)
    {
        Modifier m;
        m.Type = type;
        if (type == ModifierType::Bevel) m.Segments = 1;
        return m;
    }

    glm::mat4 Modifier::OperandMatrix() const
    {
        return glm::translate(glm::mat4(1.0f), OperandPosition) * glm::mat4_cast(glm::quat(glm::radians(OperandRotation))) * glm::scale(glm::mat4(1.0f), OperandScale);
    }

    json Modifier::ToJson() const
    {
        json j;
        j["type"] = kTypeKeys[std::clamp((int)Type, 0, (int)ModifierType::Count - 1)];
        if (!Enabled) j["enabled"] = false;
        switch (Type)
        {
        case ModifierType::Subdivision:
            j["levels"] = Levels;
            j["creases"] = UseCreases;
            break;
        case ModifierType::Mirror:
            j["axes"] = json::array({ Axis[0], Axis[1], Axis[2] });
            j["merge"] = Merge;
            j["mergeDistance"] = MergeDistance;
            break;
        case ModifierType::Array:
            j["count"] = Count;
            j["relative"] = UseRelativeOffset;
            j["relativeOffset"] = Vec(RelativeOffset);
            j["constant"] = UseConstantOffset;
            j["constantOffset"] = Vec(ConstantOffset);
            j["merge"] = Merge;
            j["mergeDistance"] = MergeDistance;
            break;
        case ModifierType::Bevel:
            j["width"] = Width;
            j["segments"] = Segments;
            j["angle"] = AngleLimit;
            break;
        case ModifierType::Solidify:
            j["thickness"] = Thickness;
            j["offset"] = Offset;
            j["rim"] = FillRim;
            break;
        case ModifierType::Boolean:
            j["operation"] = Operation;
            j["operand"] = Operand;
            j["position"] = Vec(OperandPosition);
            j["rotation"] = Vec(OperandRotation);
            j["scale"] = Vec(OperandScale);
            break;
        case ModifierType::Decimate: j["ratio"] = Ratio; break;
        case ModifierType::Weld: j["mergeDistance"] = MergeDistance; break;
        case ModifierType::Displace:
            j["strength"] = Strength;
            j["midlevel"] = Midlevel;
            j["size"] = NoiseSize;
            j["direction"] = Direction;
            j["seed"] = Seed;
            break;
        default: break;
        }
        return j;
    }

    bool Modifier::FromJson(const json& j, Modifier& out)
    {
        if (!j.is_object()) return false;
        std::string type = j.value("type", "");
        int t = -1;
        for (int i = 0; i < (int)ModifierType::Count; ++i)
            if (type == kTypeKeys[i]) t = i;
        if (t < 0) return false;
        out = Make((ModifierType)t);
        out.Enabled = j.value("enabled", true);
        out.Levels = std::clamp(j.value("levels", out.Levels), 0, 6);
        out.UseCreases = j.value("creases", out.UseCreases);
        if (j.contains("axes") && j["axes"].is_array() && j["axes"].size() == 3)
            for (int i = 0; i < 3; ++i) out.Axis[i] = j["axes"][i].get<bool>();
        out.Merge = j.value("merge", out.Merge);
        out.MergeDistance = j.value("mergeDistance", out.MergeDistance);
        out.Count = std::clamp(j.value("count", out.Count), 1, 1000);
        out.UseRelativeOffset = j.value("relative", out.UseRelativeOffset);
        out.RelativeOffset = ReadVec(j, "relativeOffset", out.RelativeOffset);
        out.UseConstantOffset = j.value("constant", out.UseConstantOffset);
        out.ConstantOffset = ReadVec(j, "constantOffset", out.ConstantOffset);
        out.Width = j.value("width", out.Width);
        out.Segments = std::clamp(j.value("segments", out.Segments), 1, 64);
        out.AngleLimit = j.value("angle", out.AngleLimit);
        out.Thickness = j.value("thickness", out.Thickness);
        out.Offset = j.value("offset", out.Offset);
        out.FillRim = j.value("rim", out.FillRim);
        out.Operation = std::clamp(j.value("operation", out.Operation), 0, 2);
        out.Operand = j.value("operand", out.Operand);
        out.OperandPosition = ReadVec(j, "position", out.OperandPosition);
        out.OperandRotation = ReadVec(j, "rotation", out.OperandRotation);
        out.OperandScale = ReadVec(j, "scale", out.OperandScale);
        out.Ratio = j.value("ratio", out.Ratio);
        out.Strength = j.value("strength", out.Strength);
        out.Midlevel = j.value("midlevel", out.Midlevel);
        out.NoiseSize = j.value("size", out.NoiseSize);
        out.Direction = std::clamp(j.value("direction", out.Direction), 0, 3);
        out.Seed = j.value("seed", out.Seed);
        return true;
    }

    // ---- Evaluation

    namespace
    {
        MeshResolver s_Resolver;
        thread_local int t_ResolveDepth = 0;

        // A copy without modifiers and selection (what a modifier works on).
        EditableMesh Plain(const EditableMesh& m)
        {
            EditableMesh out = m.EditedShape >= 0 ? m.Canonical() : m;
            out.Modifiers.clear();
            // (rigging data is carried over afterwards: EditableMesh::ToMeshData)
            out.Bones.clear();
            out.Weights.clear();
            out.ShapeKeys.clear();
            out.Actions.clear();
            return out;
        }
        void CopySettings(const EditableMesh& from, EditableMesh& to)
        {
            to.Name = from.Name;
            to.MaterialSlots = from.MaterialSlots;
            to.AutoSmooth = from.AutoSmooth;
            to.AutoSmoothAngle = from.AutoSmoothAngle;
        }
    }

    void SetMeshResolver(MeshResolver resolver) { s_Resolver = std::move(resolver); }

    namespace ModifierOps
    {
        std::vector<glm::vec3> VertexNormals(const EditableMesh& m)
        {
            std::vector<glm::vec3> n(m.VertexCount(), glm::vec3(0.0f));
            for (uint32_t f = 0; f < m.FaceCount(); ++f)
            {
                glm::vec3 fn = m.FaceNormal(f);
                for (uint32_t v : m.Faces[f].Verts) n[v] += fn;
            }
            for (auto& x : n)
                if (glm::length(x) > 1e-12f) x = glm::normalize(x);
            return n;
        }

        float Noise(const glm::vec3& p)
        {
            auto hash = [](int x, int y, int z) {
                uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
                h = (h ^ (h >> 13)) * 1274126177u;
                h ^= h >> 16;
                return (float)(h & 0xFFFFFF) / 16777215.0f;
            };
            glm::vec3 f = glm::floor(p);
            glm::vec3 t = p - f;
            t = t * t * (3.0f - 2.0f * t);
            int x = (int)f.x, y = (int)f.y, z = (int)f.z;
            float c[2][2][2];
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    for (int k = 0; k < 2; ++k) c[i][j][k] = hash(x + i, y + j, z + k);
            auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };
            float x00 = lerp(c[0][0][0], c[1][0][0], t.x), x10 = lerp(c[0][1][0], c[1][1][0], t.x);
            float x01 = lerp(c[0][0][1], c[1][0][1], t.x), x11 = lerp(c[0][1][1], c[1][1][1], t.x);
            return lerp(lerp(x00, x10, t.y), lerp(x01, x11, t.y), t.z);
        }

        EditableMesh SubdivideSmooth(const EditableMesh& m, bool useCreases)
        {
            const Topology& t = m.Topo();
            uint32_t nV = m.VertexCount(), nE = (uint32_t)t.Edges.size(), nF = m.FaceCount();
            EditableMesh out;
            CopySettings(m, out);
            std::vector<glm::vec3> facePt(nF, glm::vec3(0.0f));
            for (uint32_t f = 0; f < nF; ++f)
            {
                for (uint32_t v : m.Faces[f].Verts) facePt[f] += m.Positions[v];
                if (!m.Faces[f].Verts.empty()) facePt[f] /= (float)m.Faces[f].Verts.size();
            }
            // How sharp each edge is: its crease, or fully for open borders and edges of more than two faces.
            std::vector<float> sharp(nE, 0.0f);
            for (uint32_t e = 0; e < nE; ++e)
            {
                size_t faces = t.EdgeFaces[e].size();
                if (faces != 2) sharp[e] = 1.0f;
                else if (useCreases) sharp[e] = std::clamp(m.GetEdge(t.Edges[e]).Crease, 0.0f, 1.0f);
            }
            std::vector<glm::vec3> edgePt(nE);
            for (uint32_t e = 0; e < nE; ++e)
            {
                const glm::vec3 &a = m.Positions[EdgeA(t.Edges[e])], &b = m.Positions[EdgeB(t.Edges[e])];
                glm::vec3 mid = (a + b) * 0.5f;
                if (t.EdgeFaces[e].size() == 2)
                {
                    glm::vec3 smooth = (a + b + facePt[t.EdgeFaces[e][0]] + facePt[t.EdgeFaces[e][1]]) * 0.25f;
                    edgePt[e] = glm::mix(smooth, mid, sharp[e]);
                }
                else edgePt[e] = mid;
            }
            std::vector<glm::vec3> vertPt(nV);
            for (uint32_t v = 0; v < nV; ++v)
            {
                const glm::vec3& p = m.Positions[v];
                const auto& edges = t.VertEdges[v];
                const auto& faces = t.VertFaces[v];
                vertPt[v] = p;
                if (faces.empty() || edges.empty()) continue; // wire-only vertices stay
                auto other = [&](uint32_t e) { return m.Positions[EdgeA(t.Edges[e]) == v ? EdgeB(t.Edges[e]) : EdgeA(t.Edges[e])]; };
                std::vector<uint32_t> creased;
                float creaseSum = 0.0f;
                bool border = false;
                for (uint32_t e : edges)
                {
                    if (t.EdgeFaces[e].size() != 2) border = true;
                    if (sharp[e] > 0.0f)
                    {
                        creased.push_back(e);
                        creaseSum += sharp[e];
                    }
                }
                if (border)
                {
                    // Along the open border: the curve rule on its two border edges (corners and tangles stay).
                    std::vector<uint32_t> b;
                    for (uint32_t e : edges)
                        if (t.EdgeFaces[e].size() != 2) b.push_back(e);
                    if (b.size() == 2) vertPt[v] = (6.0f * p + other(b[0]) + other(b[1])) / 8.0f;
                    continue;
                }
                float n = (float)edges.size();
                glm::vec3 Q(0.0f), R(0.0f);
                for (uint32_t f : faces) Q += facePt[f];
                Q /= (float)faces.size();
                for (uint32_t e : edges) R += (m.Positions[EdgeA(t.Edges[e])] + m.Positions[EdgeB(t.Edges[e])]) * 0.5f;
                R /= n;
                glm::vec3 smooth = (Q + 2.0f * R + (n - 3.0f) * p) / n;
                if (creased.size() < 2) vertPt[v] = smooth;
                else if (creased.size() == 2)
                    vertPt[v] = glm::mix(smooth, (6.0f * p + other(creased[0]) + other(creased[1])) / 8.0f, std::min(creaseSum / 2.0f, 1.0f));
                else vertPt[v] = glm::mix(smooth, p, std::min(creaseSum / (float)creased.size(), 1.0f));
            }
            // Vertices: the old ones moved, then one per edge, then one per face.
            for (uint32_t v = 0; v < nV; ++v) out.AddVertex(vertPt[v]);
            for (uint32_t e = 0; e < nE; ++e) out.AddVertex(edgePt[e]);
            for (uint32_t f = 0; f < nF; ++f) out.AddVertex(facePt[f]);
            for (uint32_t f = 0; f < nF; ++f)
            {
                const Face& face = m.Faces[f];
                size_t c = face.Verts.size();
                glm::vec2 uvF(0.0f);
                for (size_t i = 0; i < c; ++i) uvF += i < face.UVs.size() ? face.UVs[i] : glm::vec2(0.0f);
                uvF /= (float)std::max<size_t>(c, 1);
                auto uv = [&](size_t i) { return i < face.UVs.size() ? face.UVs[i] : glm::vec2(0.0f); };
                for (size_t i = 0; i < c; ++i)
                {
                    size_t p = (i + c - 1) % c, q = (i + 1) % c;
                    Face nf;
                    nf.Verts = { face.Verts[i], nV + t.FaceEdges[f][i], nV + nE + f, nV + t.FaceEdges[f][p] };
                    nf.UVs = { uv(i), (uv(i) + uv(q)) * 0.5f, uvF, (uv(p) + uv(i)) * 0.5f };
                    if (face.Colors.size() == c)
                    {
                        // Colors subdivide like UVs (averaged in their 0..1 values).
                        glm::vec4 cF(0.0f);
                        for (size_t k = 0; k < c; ++k) cF += UnpackRGBA8(face.Colors[k]);
                        auto col = [&](size_t k) { return UnpackRGBA8(face.Colors[k]); };
                        nf.Colors = { face.Colors[i], PackRGBA8((col(i) + col(q)) * 0.5f), PackRGBA8(cF / (float)c), PackRGBA8((col(p) + col(i)) * 0.5f) };
                    }
                    nf.Material = face.Material;
                    nf.Smooth = face.Smooth;
                    out.Faces.push_back(std::move(nf));
                }
            }
            // Wire edges split in two; edge marks carry to both halves.
            for (EdgeKey k : m.LooseEdges)
            {
                int32_t e = t.FindEdge(EdgeA(k), EdgeB(k));
                if (e < 0) continue;
                out.LooseEdges.push_back(MakeEdgeKey(EdgeA(k), nV + (uint32_t)e));
                out.LooseEdges.push_back(MakeEdgeKey(nV + (uint32_t)e, EdgeB(k)));
            }
            for (auto& [k, d] : m.EdgeAttrs)
            {
                int32_t e = t.FindEdge(EdgeA(k), EdgeB(k));
                if (e < 0) continue;
                EdgeData nd = d;
                nd.Flags = 0;
                if (nd.IsDefault()) continue;
                out.EdgeAttrs[MakeEdgeKey(EdgeA(k), nV + (uint32_t)e)] = nd;
                out.EdgeAttrs[MakeEdgeKey(nV + (uint32_t)e, EdgeB(k))] = nd;
            }
            out.Changed();
            return out;
        }

        // ---- Constructive solid geometry on BSP trees (after Evan Wallace's csg.js).
        namespace
        {
            struct CVert
            {
                glm::vec3 P;
                glm::vec2 UV;
            };
            struct CPlane
            {
                glm::vec3 N{ 0.0f };
                float W = 0.0f;
            };
            struct CPoly
            {
                std::vector<CVert> V;
                CPlane Plane;
                int32_t Material = 0;
                bool Smooth = false;
            };
            enum { Coplanar = 0, Front = 1, Back = 2, Spanning = 3 };

            void Flip(CPoly& p)
            {
                std::reverse(p.V.begin(), p.V.end());
                p.Plane.N = -p.Plane.N;
                p.Plane.W = -p.Plane.W;
            }

            void Split(const CPlane& pl, const CPoly& poly, float eps, std::vector<CPoly>& coFront, std::vector<CPoly>& coBack, std::vector<CPoly>& front,
                       std::vector<CPoly>& back)
            {
                int type = 0;
                std::vector<int> types(poly.V.size());
                for (size_t i = 0; i < poly.V.size(); ++i)
                {
                    float d = glm::dot(pl.N, poly.V[i].P) - pl.W;
                    types[i] = d < -eps ? Back : d > eps ? Front : Coplanar;
                    type |= types[i];
                }
                switch (type)
                {
                case Coplanar: (glm::dot(pl.N, poly.Plane.N) > 0.0f ? coFront : coBack).push_back(poly); break;
                case Front: front.push_back(poly); break;
                case Back: back.push_back(poly); break;
                default:
                {
                    CPoly f = poly, b = poly;
                    f.V.clear();
                    b.V.clear();
                    size_t n = poly.V.size();
                    for (size_t i = 0; i < n; ++i)
                    {
                        size_t j = (i + 1) % n;
                        int ti = types[i], tj = types[j];
                        const CVert &vi = poly.V[i], &vj = poly.V[j];
                        if (ti != Back) f.V.push_back(vi);
                        if (ti != Front) b.V.push_back(vi);
                        if ((ti | tj) == Spanning)
                        {
                            float s = (pl.W - glm::dot(pl.N, vi.P)) / glm::dot(pl.N, vj.P - vi.P);
                            CVert v{ glm::mix(vi.P, vj.P, s), glm::mix(vi.UV, vj.UV, s) };
                            f.V.push_back(v);
                            b.V.push_back(v);
                        }
                    }
                    if (f.V.size() >= 3) front.push_back(std::move(f));
                    if (b.V.size() >= 3) back.push_back(std::move(b));
                }
                }
            }

            // A BSP tree, its nodes in one array (walked without recursion: big meshes make deep trees).
            struct Bsp
            {
                struct Node
                {
                    bool HasPlane = false;
                    CPlane Plane;
                    int Front = -1, Back = -1;
                    std::vector<CPoly> Polys;
                };
                std::vector<Node> Nodes;
                float Eps;
                explicit Bsp(float eps) : Eps(eps) { Nodes.emplace_back(); }

                void Build(std::vector<CPoly> polys)
                {
                    std::vector<std::pair<int, std::vector<CPoly>>> work;
                    work.push_back({ 0, std::move(polys) });
                    while (!work.empty())
                    {
                        auto [node, list] = std::move(work.back());
                        work.pop_back();
                        if (list.empty()) continue;
                        if (!Nodes[node].HasPlane)
                        {
                            Nodes[node].Plane = list[0].Plane;
                            Nodes[node].HasPlane = true;
                        }
                        std::vector<CPoly> front, back, co;
                        CPlane pl = Nodes[node].Plane;
                        for (const CPoly& p : list) Split(pl, p, Eps, co, co, front, back);
                        for (CPoly& p : co) Nodes[node].Polys.push_back(std::move(p));
                        if (!front.empty())
                        {
                            if (Nodes[node].Front < 0)
                            {
                                Nodes[node].Front = (int)Nodes.size();
                                Nodes.emplace_back();
                            }
                            work.push_back({ Nodes[node].Front, std::move(front) });
                        }
                        if (!back.empty())
                        {
                            if (Nodes[node].Back < 0)
                            {
                                Nodes[node].Back = (int)Nodes.size();
                                Nodes.emplace_back();
                            }
                            work.push_back({ Nodes[node].Back, std::move(back) });
                        }
                    }
                }

                // The parts of the polygons outside this tree's solid.
                std::vector<CPoly> Clip(std::vector<CPoly> polys) const
                {
                    std::vector<CPoly> out;
                    std::vector<std::pair<int, std::vector<CPoly>>> work;
                    work.push_back({ 0, std::move(polys) });
                    while (!work.empty())
                    {
                        auto [node, list] = std::move(work.back());
                        work.pop_back();
                        const Node& n = Nodes[node];
                        if (!n.HasPlane)
                        {
                            for (CPoly& p : list) out.push_back(std::move(p));
                            continue;
                        }
                        std::vector<CPoly> front, back;
                        for (const CPoly& p : list) Split(n.Plane, p, Eps, front, back, front, back);
                        if (!front.empty())
                        {
                            if (n.Front >= 0) work.push_back({ n.Front, std::move(front) });
                            else
                                for (CPoly& p : front) out.push_back(std::move(p));
                        }
                        if (!back.empty() && n.Back >= 0) work.push_back({ n.Back, std::move(back) });
                    }
                    return out;
                }

                void ClipTo(const Bsp& other)
                {
                    for (Node& n : Nodes) n.Polys = other.Clip(std::move(n.Polys));
                }
                void Invert()
                {
                    for (Node& n : Nodes)
                    {
                        for (CPoly& p : n.Polys) Flip(p);
                        n.Plane.N = -n.Plane.N;
                        n.Plane.W = -n.Plane.W;
                        std::swap(n.Front, n.Back);
                    }
                }
                std::vector<CPoly> All() const
                {
                    std::vector<CPoly> out;
                    for (const Node& n : Nodes) out.insert(out.end(), n.Polys.begin(), n.Polys.end());
                    return out;
                }
            };

            // Vertices lying on another face's border edge (T-junctions, which BSP splits leave) become corners of that
            // face too, so the faces share edges again (a watertight mesh later modifiers can work on).
            void FixTJunctions(EditableMesh& m, float eps)
            {
                const Topology& t = m.Topo();
                std::vector<uint32_t> border;
                std::vector<uint8_t> onBorder(m.VertexCount(), 0);
                for (size_t e = 0; e < t.Edges.size(); ++e)
                    if (t.EdgeFaces[e].size() == 1) onBorder[EdgeA(t.Edges[e])] = onBorder[EdgeB(t.Edges[e])] = 1;
                for (uint32_t v = 0; v < m.VertexCount(); ++v)
                    if (onBorder[v]) border.push_back(v);
                if (border.empty()) return;
                std::sort(border.begin(), border.end(), [&](uint32_t a, uint32_t b) { return m.Positions[a].x < m.Positions[b].x; });
                std::vector<float> xs;
                for (uint32_t v : border) xs.push_back(m.Positions[v].x);
                // Per face corner: the vertices to insert after it, by distance along the edge.
                std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<float, uint32_t>>> inserts;
                for (size_t e = 0; e < t.Edges.size(); ++e)
                {
                    if (t.EdgeFaces[e].size() != 1) continue;
                    uint32_t f = t.EdgeFaces[e][0];
                    const Face& face = m.Faces[f];
                    uint32_t corner = 0;
                    size_t n = face.Verts.size();
                    for (uint32_t c = 0; c < n; ++c)
                        if (t.FaceEdges[f][c] == e) corner = c;
                    uint32_t a = face.Verts[corner], b = face.Verts[(corner + 1) % n];
                    glm::vec3 pa = m.Positions[a], pb = m.Positions[b], d = pb - pa;
                    float len2 = glm::dot(d, d);
                    if (len2 < eps * eps) continue;
                    float lo = std::min(pa.x, pb.x) - eps, hi = std::max(pa.x, pb.x) + eps;
                    auto first = std::lower_bound(xs.begin(), xs.end(), lo);
                    for (size_t i = (size_t)(first - xs.begin()); i < xs.size() && xs[i] <= hi; ++i)
                    {
                        uint32_t v = border[i];
                        if (v == a || v == b) continue;
                        glm::vec3 pv = m.Positions[v];
                        float s = glm::dot(pv - pa, d) / len2;
                        if (s <= 1e-4f || s >= 1.0f - 1e-4f) continue;
                        if (glm::length(pa + d * s - pv) > eps) continue;
                        inserts[{ f, corner }].push_back({ s, v });
                    }
                }
                if (inserts.empty()) return;
                std::map<uint32_t, std::vector<uint32_t>> byFace;
                for (auto& [key, list] : inserts) byFace[key.first].push_back(key.second);
                for (auto& [f, corners] : byFace)
                {
                    Face& face = m.Faces[f];
                    Face out = face;
                    out.Verts.clear();
                    out.UVs.clear();
                    size_t n = face.Verts.size();
                    for (uint32_t c = 0; c < n; ++c)
                    {
                        glm::vec2 ua = c < face.UVs.size() ? face.UVs[c] : glm::vec2(0.0f);
                        glm::vec2 ub = (c + 1) % n < face.UVs.size() ? face.UVs[(c + 1) % n] : glm::vec2(0.0f);
                        out.Verts.push_back(face.Verts[c]);
                        out.UVs.push_back(ua);
                        auto it = inserts.find({ f, c });
                        if (it == inserts.end()) continue;
                        auto list = it->second;
                        std::sort(list.begin(), list.end());
                        for (auto& [s, v] : list)
                            if (out.Verts.back() != v)
                            {
                                out.Verts.push_back(v);
                                out.UVs.push_back(glm::mix(ua, ub, s));
                            }
                    }
                    face = std::move(out);
                }
                m.Changed();
            }

            std::vector<CPoly> ToPolys(const EditableMesh& m, uint32_t firstFace, uint32_t endFace)
            {
                std::vector<CPoly> out;
                std::vector<uint32_t> tri;
                for (uint32_t f = firstFace; f < endFace; ++f)
                {
                    const Face& face = m.Faces[f];
                    tri.clear();
                    m.TriangulateFace(f, tri);
                    for (size_t i = 0; i + 2 < tri.size(); i += 3)
                    {
                        CPoly p;
                        for (int k = 0; k < 3; ++k)
                        {
                            uint32_t c = tri[i + k];
                            p.V.push_back({ m.Positions[face.Verts[c]], c < face.UVs.size() ? face.UVs[c] : glm::vec2(0.0f) });
                        }
                        glm::vec3 n = glm::cross(p.V[1].P - p.V[0].P, p.V[2].P - p.V[0].P);
                        if (glm::length(n) < 1e-12f) continue;
                        p.Plane.N = glm::normalize(n);
                        p.Plane.W = glm::dot(p.Plane.N, p.V[0].P);
                        p.Material = face.Material;
                        p.Smooth = face.Smooth;
                        out.push_back(std::move(p));
                    }
                }
                return out;
            }
        }

        EditableMesh BooleanOp(const EditableMesh& a, const EditableMesh& b, int operation)
        {
            // Both in one mesh first, so b's material slots join a's.
            EditableMesh both = Plain(a);
            uint32_t split = both.FaceCount();
            both.Append(Plain(b));
            AABB box = both.Bounds();
            float size = std::max(glm::length(box.Max - box.Min), 1e-3f);
            float eps = 1e-5f * std::max(size, 1.0f);
            Bsp A(eps), B(eps);
            A.Build(ToPolys(both, 0, split));
            B.Build(ToPolys(both, split, both.FaceCount()));
            switch (operation)
            {
            case 1: // union
                A.ClipTo(B);
                B.ClipTo(A);
                B.Invert();
                B.ClipTo(A);
                B.Invert();
                A.Build(B.All());
                break;
            case 2: // intersection
                A.Invert();
                B.ClipTo(A);
                B.Invert();
                A.ClipTo(B);
                B.ClipTo(A);
                A.Build(B.All());
                A.Invert();
                break;
            default: // difference
                A.Invert();
                A.ClipTo(B);
                B.ClipTo(A);
                B.Invert();
                B.ClipTo(A);
                B.Invert();
                A.Build(B.All());
                A.Invert();
                break;
            }
            EditableMesh out;
            CopySettings(a, out);
            out.MaterialSlots = both.MaterialSlots;
            for (const CPoly& p : A.All())
            {
                std::vector<uint32_t> verts;
                std::vector<glm::vec2> uvs;
                for (const CVert& v : p.V)
                {
                    verts.push_back(out.AddVertex(v.P));
                    uvs.push_back(v.UV);
                }
                out.Faces.push_back({ std::move(verts), std::move(uvs), p.Material, p.Smooth, 0 });
            }
            out.Changed();
            Ops::MergeByDistance(out, eps * 10.0f, false);
            // Watertight again, and the flat fragments the splits left joined into n-gons.
            FixTJunctions(out, eps * 10.0f);
            Ops::LimitedDissolve(out, 0.05f);
            Ops::DeselectAll(out);
            return out;
        }
    }

    EditableMesh ApplyModifier(const EditableMesh& input, const Modifier& mod)
    {
        EditableMesh m = Plain(input);
        Ops::DeselectAll(m);
        switch (mod.Type)
        {
        case ModifierType::Subdivision:
            for (int i = 0; i < std::clamp(mod.Levels, 0, 6); ++i) m = ModifierOps::SubdivideSmooth(m, mod.UseCreases);
            return m;
        case ModifierType::Mirror:
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!mod.Axis[axis]) continue;
                EditableMesh copy = m;
                for (auto& p : copy.Positions) p[axis] = -p[axis];
                for (auto& f : copy.Faces)
                {
                    f.Reverse();
                }
                copy.Changed();
                uint32_t base = m.Append(copy);
                if (!mod.Merge) continue;
                std::vector<uint32_t> remap(m.VertexCount());
                std::iota(remap.begin(), remap.end(), 0u);
                for (uint32_t v = 0; v < base; ++v)
                    if (std::abs(m.Positions[v][axis]) <= mod.MergeDistance)
                    {
                        m.Positions[v][axis] = 0.0f;
                        remap[base + v] = v;
                    }
                Ops::RemapVertices(m, remap);
            }
            return m;
        case ModifierType::Array:
        {
            AABB box = m.Bounds();
            glm::vec3 offset(0.0f);
            if (mod.UseRelativeOffset) offset += mod.RelativeOffset * (box.Max - box.Min);
            if (mod.UseConstantOffset) offset += mod.ConstantOffset;
            EditableMesh one = m;
            for (int i = 1; i < std::max(mod.Count, 1); ++i)
            {
                EditableMesh copy = one;
                for (auto& p : copy.Positions) p += offset * (float)i;
                copy.Changed();
                m.Append(copy);
            }
            if (mod.Merge) Ops::MergeByDistance(m, mod.MergeDistance, false);
            return m;
        }
        case ModifierType::Bevel:
        {
            const Topology& t = m.Topo();
            float limit = std::cos(glm::radians(std::clamp(mod.AngleLimit, 0.0f, 180.0f)));
            std::vector<EdgeKey> pick;
            for (size_t e = 0; e < t.Edges.size(); ++e)
            {
                const auto& ef = t.EdgeFaces[e];
                if (ef.size() != 2) continue;
                if (mod.AngleLimit > 0.0f && glm::dot(m.FaceNormal(ef[0]), m.FaceNormal(ef[1])) > limit) continue;
                pick.push_back(t.Edges[e]);
            }
            for (EdgeKey k : pick)
            {
                Ops::SelectEdge(m, k, true);
                Ops::SelectVertex(m, EdgeA(k), true);
                Ops::SelectVertex(m, EdgeB(k), true);
            }
            Ops::Bevel(m, mod.Width, (uint32_t)std::max(mod.Segments, 1));
            Ops::DeselectAll(m);
            return m;
        }
        case ModifierType::Solidify:
        {
            std::vector<glm::vec3> normals = ModifierOps::VertexNormals(m);
            float o = std::clamp(mod.Offset, -1.0f, 1.0f);
            float outer = mod.Thickness * (o + 1.0f) * 0.5f, inner = mod.Thickness * (o - 1.0f) * 0.5f;
            uint32_t nV = m.VertexCount(), nF = m.FaceCount();
            const Topology t = m.Topo();
            std::vector<uint32_t> back(nV, UINT32_MAX);
            std::vector<glm::vec3> start = m.Positions;
            for (uint32_t v = 0; v < nV; ++v)
            {
                if (t.VertFaces[v].empty()) continue;
                m.Positions[v] = start[v] + normals[v] * outer;
                back[v] = m.AddVertex(start[v] + normals[v] * inner);
            }
            for (uint32_t f = 0; f < nF; ++f)
            {
                Face copy = m.Faces[f];
                for (uint32_t& v : copy.Verts) v = back[v];
                copy.Reverse();
                m.Faces.push_back(std::move(copy));
            }
            std::vector<std::pair<EdgeKey, EdgeData>> marks(m.EdgeAttrs.begin(), m.EdgeAttrs.end());
            for (auto& [k, d] : marks)
                if (back[EdgeA(k)] != UINT32_MAX && back[EdgeB(k)] != UINT32_MAX) m.EdgeAttrs[MakeEdgeKey(back[EdgeA(k)], back[EdgeB(k)])] = d;
            if (mod.FillRim)
                for (size_t e = 0; e < t.Edges.size(); ++e)
                {
                    if (t.EdgeFaces[e].size() != 1) continue;
                    const Face& face = m.Faces[t.EdgeFaces[e][0]];
                    uint32_t a = EdgeA(t.Edges[e]), b = EdgeB(t.Edges[e]);
                    for (size_t i = 0, n = face.Verts.size(); i < n; ++i)
                        if (face.Verts[i] == b && face.Verts[(i + 1) % n] == a) std::swap(a, b); // so the face walks a -> b
                    Face rim;
                    rim.Verts = { b, a, back[a], back[b] };
                    rim.UVs = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
                    rim.Material = face.Material;
                    rim.Smooth = face.Smooth;
                    m.Faces.push_back(std::move(rim));
                }
            m.Changed();
            return m;
        }
        case ModifierType::Boolean:
        {
            if (!s_Resolver || mod.Operand == 0 || t_ResolveDepth > 4) return m;
            EditableMesh other;
            ++t_ResolveDepth;
            bool found = s_Resolver(mod.Operand, other);
            --t_ResolveDepth;
            if (!found || other.FaceCount() == 0) return m;
            glm::mat4 xf = mod.OperandMatrix();
            for (auto& p : other.Positions) p = glm::vec3(xf * glm::vec4(p, 1.0f));
            if (glm::determinant(glm::mat3(xf)) < 0.0f)
                for (auto& f : other.Faces)
                {
                    f.Reverse();
                }
            other.Changed();
            return ModifierOps::BooleanOp(m, other, mod.Operation);
        }
        case ModifierType::Decimate:
        {
            if (mod.Ratio >= 1.0f || m.FaceCount() == 0) return m;
            MeshData simple = MeshSimplify::Simplify(m.ToMeshData(false), std::clamp(mod.Ratio, 0.01f, 1.0f), 1.0f);
            EditableMesh out = EditableMesh::FromMeshData(simple, false);
            CopySettings(m, out);
            Ops::DeselectAll(out);
            return out;
        }
        case ModifierType::Weld: Ops::MergeByDistance(m, mod.MergeDistance, false); return m;
        case ModifierType::Displace:
        {
            std::vector<glm::vec3> normals = ModifierOps::VertexNormals(m);
            glm::vec3 seed((float)mod.Seed * 17.31f, (float)mod.Seed * 9.17f, (float)mod.Seed * 3.71f);
            float size = std::max(mod.NoiseSize, 1e-4f);
            for (uint32_t v = 0; v < m.VertexCount(); ++v)
            {
                float d = (ModifierOps::Noise(m.Positions[v] / size + seed) - mod.Midlevel) * mod.Strength;
                glm::vec3 dir = normals[v];
                if (mod.Direction > 0)
                {
                    dir = glm::vec3(0.0f);
                    dir[mod.Direction - 1] = 1.0f;
                }
                m.Positions[v] += dir * d;
            }
            m.Changed();
            return m;
        }
        default: return m;
        }
    }

    EditableMesh EvaluateModifiers(const EditableMesh& mesh)
    {
        EditableMesh m = Plain(mesh);
        for (const Modifier& mod : mesh.Modifiers)
            if (mod.Enabled) m = ApplyModifier(m, mod);
        return m;
    }
}
