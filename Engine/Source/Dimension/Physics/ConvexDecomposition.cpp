#include "Dimension/Physics/ConvexDecomposition.h"
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Project/Project.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>

#define ENABLE_VHACD_IMPLEMENTATION 1
#pragma warning(push, 0)
#include <VHACD.h>
#pragma warning(pop)

namespace Dimension::ConvexDecomposition
{
    namespace
    {
        struct Entry
        {
            std::shared_ptr<const MeshData> Mesh; // keeps the key's address from being reused
            std::shared_ptr<const std::vector<ConvexPart>> Parts;
        };
        std::mutex s_Mutex;
        std::map<std::tuple<const MeshData*, int, int>, Entry> s_Cache;

        // Results also persist in the project's .dimension/Cache/Convex (shipped with builds), keyed by a hash of the
        // mesh's triangles and the settings, so each mesh is decomposed once.
        uint64_t ContentHash(const MeshData& mesh, int maxParts, int resolution)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const void* data, size_t size) {
                for (size_t i = 0; i < size; ++i) h = (h ^ ((const uint8_t*)data)[i]) * 1099511628211ull;
            };
            for (auto& v : mesh.Vertices) mix(&v.Position, sizeof(v.Position));
            mix(mesh.Indices.data(), mesh.Indices.size() * sizeof(uint32_t));
            int params[3] = { maxParts, resolution, 1 }; // 1: format version
            mix(params, sizeof(params));
            return h;
        }

        fs::path CacheFile(uint64_t hash)
        {
            const fs::path& project = Project::GetDirectory();
            if (project.empty()) return {};
            char name[32];
            std::snprintf(name, sizeof(name), "%016llx.hulls", (unsigned long long)hash);
            return project / ".dimension" / "Cache" / "Convex" / name;
        }

        bool ReadCache(const fs::path& file, std::vector<ConvexPart>& parts)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) return false;
            auto read = [&](void* p, size_t n) { return (bool)in.read((char*)p, (std::streamsize)n); };
            uint32_t count = 0;
            if (!read(&count, 4) || count > 4096) return false;
            parts.resize(count);
            for (auto& part : parts)
            {
                uint32_t points = 0, indices = 0;
                if (!read(&points, 4) || points > 1u << 20) return false;
                part.Points.resize(points);
                if (!read(part.Points.data(), points * sizeof(glm::vec3)) || !read(&indices, 4) || indices > 1u << 22) return false;
                part.Triangles.resize(indices);
                if (!read(part.Triangles.data(), indices * sizeof(uint32_t))) return false;
            }
            return true;
        }

        void WriteCache(const fs::path& file, const std::vector<ConvexPart>& parts)
        {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary);
            auto write = [&](const void* p, size_t n) { out.write((const char*)p, (std::streamsize)n); };
            uint32_t count = (uint32_t)parts.size();
            write(&count, 4);
            for (auto& part : parts)
            {
                uint32_t points = (uint32_t)part.Points.size(), indices = (uint32_t)part.Triangles.size();
                write(&points, 4);
                write(part.Points.data(), points * sizeof(glm::vec3));
                write(&indices, 4);
                write(part.Triangles.data(), indices * sizeof(uint32_t));
            }
        }
    }

    std::shared_ptr<const std::vector<ConvexPart>> Decompose(const std::shared_ptr<const MeshData>& mesh, int maxParts, int resolution)
    {
        if (!mesh || mesh->Vertices.empty() || mesh->Indices.size() < 3) return std::make_shared<std::vector<ConvexPart>>();
        maxParts = std::clamp(maxParts, 1, 256);
        resolution = std::clamp(resolution, 5000, 10000000);
        auto key = std::make_tuple(mesh.get(), maxParts, resolution);
        {
            std::lock_guard lock(s_Mutex);
            if (auto it = s_Cache.find(key); it != s_Cache.end()) return it->second.Parts;
        }

        fs::path file = CacheFile(ContentHash(*mesh, maxParts, resolution));
        if (auto cached = std::make_shared<std::vector<ConvexPart>>(); !file.empty() && ReadCache(file, *cached))
        {
            std::lock_guard lock(s_Mutex);
            s_Cache[key] = { mesh, cached };
            return cached;
        }

        auto start = std::chrono::steady_clock::now();
        std::vector<float> points;
        points.reserve(mesh->Vertices.size() * 3);
        for (auto& v : mesh->Vertices) points.insert(points.end(), { v.Position.x, v.Position.y, v.Position.z });
        VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
        VHACD::IVHACD::Parameters params;
        params.m_maxConvexHulls = (uint32_t)maxParts;
        params.m_resolution = (uint32_t)resolution;
        params.m_maxNumVerticesPerCH = 64;
        params.m_asyncACD = false;
        params.m_logger = nullptr;
        auto parts = std::make_shared<std::vector<ConvexPart>>();
        if (vhacd->Compute(points.data(), (uint32_t)mesh->Vertices.size(), mesh->Indices.data(), (uint32_t)(mesh->Indices.size() / 3), params))
            for (uint32_t i = 0; i < vhacd->GetNConvexHulls(); ++i)
            {
                VHACD::IVHACD::ConvexHull hull;
                if (!vhacd->GetConvexHull(i, hull) || hull.m_points.size() < 4) continue;
                ConvexPart part;
                for (auto& p : hull.m_points) part.Points.push_back({ (float)p.mX, (float)p.mY, (float)p.mZ });
                for (auto& t : hull.m_triangles) part.Triangles.insert(part.Triangles.end(), { t.mI0, t.mI1, t.mI2 });
                parts->push_back(std::move(part));
            }
        vhacd->Release();
        float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        Log::Info("Convex decomposition of '{}': {} part(s) in {:.0f} ms", mesh->Name, parts->size(), ms);
        if (!file.empty()) WriteCache(file, *parts);

        std::lock_guard lock(s_Mutex);
        s_Cache[key] = { mesh, parts };
        return parts;
    }

    void ClearCache()
    {
        std::lock_guard lock(s_Mutex);
        s_Cache.clear();
    }
}
