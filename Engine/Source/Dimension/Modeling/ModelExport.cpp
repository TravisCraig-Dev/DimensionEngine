#include "Dimension/Modeling/ModelExport.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace Dimension::Modeling
{
    using json = nlohmann::json;

    namespace
    {
        std::string Base64(const std::vector<uint8_t>& data)
        {
            static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((data.size() + 2) / 3 * 4);
            for (size_t i = 0; i < data.size(); i += 3)
            {
                uint32_t n = (uint32_t)data[i] << 16;
                if (i + 1 < data.size()) n |= (uint32_t)data[i + 1] << 8;
                if (i + 2 < data.size()) n |= data[i + 2];
                out += k[(n >> 18) & 63];
                out += k[(n >> 12) & 63];
                out += i + 1 < data.size() ? k[(n >> 6) & 63] : '=';
                out += i + 2 < data.size() ? k[n & 63] : '=';
            }
            return out;
        }

        template <class T> size_t Append(std::vector<uint8_t>& bin, const T* data, size_t count)
        {
            while (bin.size() % 4) bin.push_back(0);
            size_t offset = bin.size();
            bin.resize(offset + sizeof(T) * count);
            if (count) std::memcpy(bin.data() + offset, data, sizeof(T) * count);
            return offset;
        }
    }

    bool ExportGltf(const MeshData& mesh, const fs::path& path)
    {
        if (mesh.Vertices.empty() || mesh.Indices.empty())
        {
            Log::Error("Export '{}': the mesh is empty", path.filename().string());
            return false;
        }
        bool tangents = false;
        for (const Vertex& v : mesh.Vertices) tangents |= v.Tangent != glm::vec4(0.0f);

        std::vector<uint8_t> bin;
        json views = json::array(), accessors = json::array();
        auto view = [&](size_t offset, size_t length, int target) {
            views.push_back({ { "buffer", 0 }, { "byteOffset", offset }, { "byteLength", length }, { "target", target } });
            return views.size() - 1;
        };
        auto accessor = [&](size_t v, int componentType, size_t count, const char* type) {
            accessors.push_back({ { "bufferView", v }, { "componentType", componentType }, { "count", count }, { "type", type } });
            return accessors.size() - 1;
        };
        constexpr int kFloat = 5126, kUInt = 5125, kArray = 34962, kElements = 34963;

        std::vector<SubMesh> subs = mesh.SubMeshes;
        if (subs.empty()) subs.push_back({ 0, (uint32_t)mesh.Indices.size(), -1 });
        json materials = json::array();
        for (const auto& m : mesh.Materials)
        {
            json mat = { { "name", m.Name.empty() ? "Material" : m.Name } };
            mat["pbrMetallicRoughness"] = { { "metallicFactor", 0.0 }, { "roughnessFactor", 0.5 } };
            materials.push_back(mat);
        }
        // A primitive per sub-mesh, each with its own vertices (the ones its triangles use, in first-use order).
        json primitives = json::array();
        std::vector<uint32_t> local(mesh.Vertices.size());
        for (const SubMesh& sm : subs)
        {
            if (sm.IndexCount == 0 || (size_t)sm.IndexOffset + sm.IndexCount > mesh.Indices.size()) continue;
            std::fill(local.begin(), local.end(), UINT32_MAX);
            std::vector<uint32_t> indices, used;
            for (uint32_t i = sm.IndexOffset; i < sm.IndexOffset + sm.IndexCount; ++i)
            {
                uint32_t v = mesh.Indices[i];
                if (local[v] == UINT32_MAX)
                {
                    local[v] = (uint32_t)used.size();
                    used.push_back(v);
                }
                indices.push_back(local[v]);
            }
            size_t n = used.size();
            std::vector<glm::vec3> pos(n), nrm(n);
            std::vector<glm::vec2> uv(n);
            std::vector<glm::vec4> tan(n);
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (size_t i = 0; i < n; ++i)
            {
                const Vertex& v = mesh.Vertices[used[i]];
                pos[i] = v.Position;
                float len = glm::length(v.Normal);
                nrm[i] = len > 1e-12f ? v.Normal / len : glm::vec3(0, 1, 0);
                uv[i] = v.UV;
                glm::vec3 t(v.Tangent);
                float tl = glm::length(t);
                tan[i] = tl > 1e-12f ? glm::vec4(t / tl, v.Tangent.w < 0.0f ? -1.0f : 1.0f) : glm::vec4(1, 0, 0, 1);
                mn = glm::min(mn, v.Position);
                mx = glm::max(mx, v.Position);
            }
            json attributes;
            size_t a = accessor(view(Append(bin, pos.data(), n), n * 12, kArray), kFloat, n, "VEC3");
            accessors[a]["min"] = { mn.x, mn.y, mn.z };
            accessors[a]["max"] = { mx.x, mx.y, mx.z };
            attributes["POSITION"] = a;
            attributes["NORMAL"] = accessor(view(Append(bin, nrm.data(), n), n * 12, kArray), kFloat, n, "VEC3");
            attributes["TEXCOORD_0"] = accessor(view(Append(bin, uv.data(), n), n * 8, kArray), kFloat, n, "VEC2");
            if (tangents) attributes["TANGENT"] = accessor(view(Append(bin, tan.data(), n), n * 16, kArray), kFloat, n, "VEC4");
            size_t off = Append(bin, indices.data(), indices.size());
            json prim = { { "attributes", attributes }, { "mode", 4 } };
            prim["indices"] = accessor(view(off, indices.size() * 4, kElements), kUInt, indices.size(), "SCALAR");
            if (sm.MaterialSlot >= 0 && sm.MaterialSlot < (int32_t)materials.size()) prim["material"] = sm.MaterialSlot;
            primitives.push_back(prim);
        }
        while (bin.size() % 4) bin.push_back(0);

        std::string name = mesh.Name.empty() ? path.stem().string() : mesh.Name;
        json gltf;
        gltf["asset"] = { { "version", "2.0" }, { "generator", "Dimension Engine" } };
        gltf["scene"] = 0;
        gltf["scenes"] = json::array({ { { "nodes", json::array({ 0 }) } } });
        gltf["nodes"] = json::array({ { { "name", name }, { "mesh", 0 } } });
        gltf["meshes"] = json::array({ { { "name", name }, { "primitives", primitives } } });
        if (!materials.empty()) gltf["materials"] = materials;
        gltf["accessors"] = accessors;
        gltf["bufferViews"] = views;

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            Log::Error("Export: cannot write '{}'", path.string());
            return false;
        }
        if (ext == ".gltf")
        {
            gltf["buffers"] = json::array({ { { "byteLength", bin.size() }, { "uri", "data:application/octet-stream;base64," + Base64(bin) } } });
            file << gltf.dump(2);
            return (bool)file;
        }
        gltf["buffers"] = json::array({ { { "byteLength", bin.size() } } });
        std::string text = gltf.dump();
        while (text.size() % 4) text += ' ';
        auto u32 = [&](uint32_t v) { file.write((const char*)&v, 4); };
        u32(0x46546C67); // "glTF"
        u32(2);
        u32((uint32_t)(12 + 8 + text.size() + 8 + bin.size()));
        u32((uint32_t)text.size());
        u32(0x4E4F534A); // "JSON"
        file.write(text.data(), (std::streamsize)text.size());
        u32((uint32_t)bin.size());
        u32(0x004E4942); // "BIN"
        file.write((const char*)bin.data(), (std::streamsize)bin.size());
        return (bool)file;
    }
}
