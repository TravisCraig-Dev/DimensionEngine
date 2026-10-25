#include "Dimension/Physics/Cloth.h"
#include "Dimension/Scene/Components.h"
#include <map>
#include <mutex>
#include <tuple>

namespace Dimension::Cloth
{
    namespace
    {
        int ResolutionOf(const ClothComponent& c) { return std::clamp(c.Resolution, 1, 128); }
    }

    MeshData BuildGrid(const ClothComponent& cloth)
    {
        int n = ResolutionOf(cloth);
        glm::vec2 size = glm::max(cloth.Size, glm::vec2(0.01f));
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(size_t(n + 1) * (n + 1));
        for (int j = 0; j <= n; ++j)     // rows, bottom to top
            for (int i = 0; i <= n; ++i) // columns, left to right
            {
                float u = (float)i / n, v = (float)j / n;
                Vertex vert;
                vert.Position = { (u - 0.5f) * size.x, (v - 0.5f) * size.y, 0.0f };
                vert.Normal = { 0.0f, 0.0f, 1.0f };
                vert.UV = { u, 1.0f - v };
                vert.Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
                vertices.push_back(vert);
            }
        auto at = [n](int i, int j) { return (uint32_t)(j * (n + 1) + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                indices.insert(indices.end(), { at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j), at(i + 1, j + 1), at(i, j + 1) });
        MeshData data = MeshData::FromArrays(std::move(vertices), std::move(indices), "Cloth");
        return data;
    }

    std::vector<uint32_t> PinnedVertices(const ClothComponent& cloth)
    {
        int n = ResolutionOf(cloth);
        std::vector<uint32_t> out;
        uint32_t top = (uint32_t)(n * (n + 1));
        switch (cloth.Pin)
        {
        case ClothPin::TopEdge:
            for (int i = 0; i <= n; ++i) out.push_back(top + i);
            break;
        case ClothPin::TopCorners: out = { top, top + (uint32_t)n }; break;
        case ClothPin::None: break;
        }
        return out;
    }

    namespace
    {
        struct Grid
        {
            std::vector<Vertex> Vertices;
            Ref<Mesh> GpuMesh;
        };
        std::mutex s_GridMutex;
        std::map<std::tuple<float, float, int>, Grid> s_Grids;

        Grid& GridFor(const ClothComponent& cloth, bool gpu)
        {
            Grid& g = s_Grids[std::make_tuple(cloth.Size.x, cloth.Size.y, ResolutionOf(cloth))];
            if (g.Vertices.empty()) g.Vertices = BuildGrid(cloth).Vertices;
            if (gpu && !g.GpuMesh) g.GpuMesh = MakeRef<Mesh>(BuildGrid(cloth));
            return g;
        }
    }

    Ref<Mesh> GridMesh(const ClothComponent& cloth)
    {
        std::lock_guard lock(s_GridMutex);
        return GridFor(cloth, true).GpuMesh;
    }

    void ReleaseMeshes()
    {
        std::lock_guard lock(s_GridMutex);
        s_Grids.clear();
    }

    const std::vector<Vertex>& GridVertices(const ClothComponent& cloth)
    {
        std::lock_guard lock(s_GridMutex);
        return GridFor(cloth, false).Vertices;
    }

    void RecomputeNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
    {
        std::vector<glm::vec3> normals(vertices.size(), glm::vec3(0.0f));
        for (size_t k = 0; k + 2 < indices.size(); k += 3)
        {
            uint32_t a = indices[k], b = indices[k + 1], c = indices[k + 2];
            glm::vec3 n = glm::cross(vertices[b].Position - vertices[a].Position, vertices[c].Position - vertices[a].Position); // area-weighted
            normals[a] += n;
            normals[b] += n;
            normals[c] += n;
        }
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            float len = glm::length(normals[i]);
            if (len > 1e-12f) vertices[i].Normal = normals[i] / len;
            // Tangent: the grid's +U direction projected onto the surface.
            glm::vec3 t = glm::vec3(vertices[i].Tangent) - vertices[i].Normal * glm::dot(glm::vec3(vertices[i].Tangent), vertices[i].Normal);
            if (glm::dot(t, t) > 1e-12f) vertices[i].Tangent = glm::vec4(glm::normalize(t), 1.0f);
        }
    }
}
