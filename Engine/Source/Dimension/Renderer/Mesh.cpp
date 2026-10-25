#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Core/Log.h"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include "Dimension/Renderer/RenderDevice.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace Dimension
{
    namespace
    {
        // Ensures every triangle winds counter-clockwise when seen from the side its
        // vertex normals point to. Lets the generators focus on geometry.
        void FixWinding(const std::vector<Vertex>& v, std::vector<uint32_t>& idx)
        {
            for (size_t i = 0; i + 2 < idx.size(); i += 3)
            {
                const Vertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
                glm::vec3 face = glm::cross(b.Position - a.Position, c.Position - a.Position);
                if (glm::dot(face, a.Normal + b.Normal + c.Normal) < 0.0f) std::swap(idx[i + 1], idx[i + 2]);
            }
        }

        void ComputeNormals(std::vector<Vertex>& v, const std::vector<uint32_t>& idx)
        {
            for (auto& vert : v) vert.Normal = glm::vec3(0.0f);
            for (size_t i = 0; i + 2 < idx.size(); i += 3)
            {
                Vertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
                glm::vec3 n = glm::cross(b.Position - a.Position, c.Position - a.Position);
                a.Normal += n;
                b.Normal += n;
                c.Normal += n;
            }
            for (auto& vert : v)
                vert.Normal = glm::length(vert.Normal) > 0.0f ? glm::normalize(vert.Normal) : glm::vec3(0, 1, 0);
        }

        glm::mat4 NodeLocal(const cgltf_node* node)
        {
            float m[16];
            cgltf_node_transform_local(node, m);
            return glm::make_mat4(m);
        }

        glm::mat4 NodeWorld(const cgltf_node* node)
        {
            float m[16];
            cgltf_node_transform_world(node, m);
            return glm::make_mat4(m);
        }

        void ReadPrimitive(const cgltf_primitive& prim, const glm::mat4& transform, MeshData& out, bool& needNormals,
                           const std::vector<int>* jointRemap)
        {
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *joints = nullptr, *weights = nullptr, *tan = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a)
            {
                auto& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
                else if (attr.type == cgltf_attribute_type_tangent) tan = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
                else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) joints = attr.data;
                else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) weights = attr.data;
            }
            if (!pos) return;
            needNormals |= nrm == nullptr;
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
            uint32_t base = (uint32_t)out.Vertices.size();
            for (cgltf_size i = 0; i < pos->count; ++i)
            {
                Vertex v{};
                cgltf_accessor_read_float(pos, i, glm::value_ptr(v.Position), 3);
                v.Position = glm::vec3(transform * glm::vec4(v.Position, 1.0f));
                if (nrm)
                {
                    cgltf_accessor_read_float(nrm, i, glm::value_ptr(v.Normal), 3);
                    v.Normal = glm::normalize(normalMatrix * v.Normal);
                }
                if (uv) cgltf_accessor_read_float(uv, i, glm::value_ptr(v.UV), 2);
                if (tan && nrm)
                {
                    // Authored tangents (glTF TANGENT): match the artist's normal-map baker exactly.
                    glm::vec4 t;
                    cgltf_accessor_read_float(tan, i, glm::value_ptr(t), 4);
                    glm::vec3 tw = glm::mat3(transform) * glm::vec3(t);
                    float handedness = (t.w < 0.0f ? -1.0f : 1.0f) * (glm::determinant(glm::mat3(transform)) < 0.0f ? -1.0f : 1.0f);
                    if (glm::dot(tw, tw) > 1e-12f) v.Tangent = glm::vec4(glm::normalize(tw), handedness);
                }
                out.Vertices.push_back(v);

                if (jointRemap)
                {
                    SkinVertex s;
                    if (joints && weights)
                    {
                        cgltf_uint j[4] = {};
                        cgltf_accessor_read_uint(joints, i, j, 4);
                        cgltf_accessor_read_float(weights, i, glm::value_ptr(s.Weights), 4);
                        for (int k = 0; k < 4; ++k)
                            s.Joints[k] = j[k] < jointRemap->size() ? (uint32_t)std::max(0, (*jointRemap)[j[k]]) : 0u;
                        float sum = s.Weights.x + s.Weights.y + s.Weights.z + s.Weights.w;
                        s.Weights = sum > 0.0f ? s.Weights / sum : glm::vec4(1, 0, 0, 0);
                    }
                    out.Skin.push_back(s);
                }
            }
            if (prim.indices)
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    out.Indices.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, i));
            else
                for (cgltf_size i = 0; i < pos->count; ++i) out.Indices.push_back(base + (uint32_t)i);
        }

        // Builds the skeleton (joint hierarchy, bind pose, inverse bind matrices) of a glTF skin.
        Ref<Skeleton> ReadSkeleton(const cgltf_skin& skin, std::unordered_map<const cgltf_node*, int>& jointOf)
        {
            auto skeleton = MakeRef<Skeleton>();
            for (cgltf_size j = 0; j < skin.joints_count; ++j) jointOf[skin.joints[j]] = (int)j;
            bool rootTransformSet = false;
            for (cgltf_size j = 0; j < skin.joints_count; ++j)
            {
                const cgltf_node* node = skin.joints[j];
                Joint joint;
                joint.Name = node->name ? node->name : ("Joint" + std::to_string(j));
                // Nearest joint ancestor (intermediate non-joint nodes are folded into RootTransform).
                for (const cgltf_node* p = node->parent; p; p = p->parent)
                    if (auto it = jointOf.find(p); it != jointOf.end())
                    {
                        joint.Parent = it->second;
                        break;
                    }
                if (joint.Parent < 0 && !rootTransformSet && node->parent)
                {
                    skeleton->RootTransform = NodeWorld(node->parent);
                    rootTransformSet = true;
                }
                if (node->has_translation) joint.Translation = glm::make_vec3(node->translation);
                if (node->has_rotation) joint.Rotation = glm::quat(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]);
                if (node->has_scale) joint.Scale = glm::make_vec3(node->scale);
                if (node->has_matrix)
                {
                    glm::mat4 m = glm::make_mat4(node->matrix);
                    glm::vec3 skew;
                    glm::vec4 persp;
                    glm::decompose(m, joint.Scale, joint.Rotation, joint.Translation, skew, persp);
                }
                if (skin.inverse_bind_matrices)
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, glm::value_ptr(joint.InverseBind), 16);
                skeleton->Joints.push_back(joint);
            }
            skeleton->ComputeOrder();
            return skeleton;
        }

        void ReadAnimations(const cgltf_data* data, const std::unordered_map<const cgltf_node*, int>& jointOf, MeshData& out)
        {
            for (cgltf_size a = 0; a < data->animations_count; ++a)
            {
                const cgltf_animation& anim = data->animations[a];
                AnimationClip clip;
                clip.Name = anim.name && *anim.name ? anim.name : ("Clip" + std::to_string(a));
                for (cgltf_size c = 0; c < anim.channels_count; ++c)
                {
                    const cgltf_animation_channel& ch = anim.channels[c];
                    auto it = jointOf.find(ch.target_node);
                    if (it == jointOf.end() || !ch.sampler) continue;
                    AnimationChannel channel;
                    channel.Joint = it->second;
                    if (ch.target_path == cgltf_animation_path_type_translation) channel.Path = AnimationPath::Translation;
                    else if (ch.target_path == cgltf_animation_path_type_rotation) channel.Path = AnimationPath::Rotation;
                    else if (ch.target_path == cgltf_animation_path_type_scale) channel.Path = AnimationPath::Scale;
                    else continue; // morph weights are not supported
                    const cgltf_animation_sampler& s = *ch.sampler;
                    channel.Interpolation = s.interpolation == cgltf_interpolation_type_step ? AnimationInterpolation::Step : AnimationInterpolation::Linear;
                    bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline; // use the value, ignore tangents
                    int comps = channel.Path == AnimationPath::Rotation ? 4 : 3;
                    for (cgltf_size k = 0; k < s.input->count; ++k)
                    {
                        float t = 0.0f;
                        cgltf_accessor_read_float(s.input, k, &t, 1);
                        glm::vec4 v(0.0f, 0.0f, 0.0f, 1.0f);
                        cgltf_accessor_read_float(s.output, cubic ? k * 3 + 1 : k, glm::value_ptr(v), comps);
                        channel.Times.push_back(t);
                        channel.Values.push_back(v);
                        clip.Duration = std::max(clip.Duration, t);
                    }
                    clip.Channels.push_back(std::move(channel));
                }
                if (!clip.Channels.empty()) out.Clips.push_back(std::move(clip));
            }
        }

        float LinearToSrgb(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        // Images and materials (glTF factors are linear; Material colors are sRGB like the editor's color pickers).
        void ReadMaterials(const cgltf_data* data, const cgltf_options& options, const fs::path& path, MeshData& out)
        {
            for (cgltf_size i = 0; i < data->images_count; ++i)
            {
                const cgltf_image& image = data->images[i];
                ImportedImage im;
                im.Name = image.name && *image.name ? image.name : "Image" + std::to_string(i);
                if (image.buffer_view && image.buffer_view->buffer->data)
                {
                    auto* bytes = (const uint8_t*)image.buffer_view->buffer->data + image.buffer_view->offset;
                    im.Bytes.assign(bytes, bytes + image.buffer_view->size);
                }
                else if (image.uri && std::strncmp(image.uri, "data:", 5) == 0)
                {
                    const char* comma = std::strchr(image.uri, ',');
                    if (comma && std::strstr(image.uri, ";base64,"))
                    {
                        size_t len = std::strlen(comma + 1);
                        size_t size = len / 4 * 3;
                        if (len >= 1 && comma[len] == '=') --size;
                        if (len >= 2 && comma[len - 1] == '=') --size;
                        void* decoded = nullptr;
                        if (cgltf_load_buffer_base64(&options, size, comma + 1, &decoded) == cgltf_result_success)
                        {
                            im.Bytes.assign((const uint8_t*)decoded, (const uint8_t*)decoded + size);
                            std::free(decoded);
                        }
                    }
                }
                else if (image.uri)
                {
                    std::string uri = image.uri;
                    cgltf_decode_uri(uri.data());
                    uri.resize(std::strlen(uri.c_str()));
                    im.File = (path.parent_path() / fs::u8path(uri)).lexically_normal();
                    if (!image.name || !*image.name) im.Name = im.File.stem().string();
                }
                out.Images.push_back(std::move(im));
            }

            auto imageOf = [&](const cgltf_texture_view& view) -> int32_t {
                if (!view.texture || !view.texture->image) return -1;
                return (int32_t)(view.texture->image - data->images);
            };
            for (cgltf_size i = 0; i < data->materials_count; ++i)
            {
                const cgltf_material& src = data->materials[i];
                ImportedMaterial m;
                m.Name = src.name && *src.name ? src.name : "Material" + std::to_string(i);
                Material& v = m.Values;
                const auto& pbr = src.pbr_metallic_roughness;
                v.BaseColor = { LinearToSrgb(pbr.base_color_factor[0]), LinearToSrgb(pbr.base_color_factor[1]),
                                LinearToSrgb(pbr.base_color_factor[2]), pbr.base_color_factor[3] };
                v.Metallic = pbr.metallic_factor;
                v.Roughness = pbr.roughness_factor;
                m.BaseColorImage = imageOf(pbr.base_color_texture);
                m.MetallicRoughnessImage = imageOf(pbr.metallic_roughness_texture);
                if (pbr.base_color_texture.has_transform)
                {
                    v.Tiling = { pbr.base_color_texture.transform.scale[0], pbr.base_color_texture.transform.scale[1] };
                    v.Offset = { pbr.base_color_texture.transform.offset[0], pbr.base_color_texture.transform.offset[1] };
                }
                m.NormalImage = imageOf(src.normal_texture);
                if (m.NormalImage >= 0) v.NormalStrength = src.normal_texture.scale;
                v.EmissionColor = { LinearToSrgb(src.emissive_factor[0]), LinearToSrgb(src.emissive_factor[1]), LinearToSrgb(src.emissive_factor[2]) };
                v.EmissionStrength = src.has_emissive_strength ? src.emissive_strength.emissive_strength : 1.0f;
                m.EmissionImage = imageOf(src.emissive_texture);
                if (src.alpha_mode == cgltf_alpha_mode_mask)
                {
                    v.BlendMode = MaterialBlendMode::Cutout;
                    v.AlphaCutoff = src.alpha_cutoff;
                }
                else if (src.alpha_mode == cgltf_alpha_mode_blend)
                    v.BlendMode = MaterialBlendMode::Transparent;
                v.DoubleSided = src.double_sided;
                out.Materials.push_back(std::move(m));
            }
        }

        bool LoadGltf(const fs::path& path, MeshData& out)
        {
            cgltf_options options{};
            cgltf_data* data = nullptr;
            std::string file = path.string();
            if (cgltf_parse_file(&options, file.c_str(), &data) != cgltf_result_success ||
                cgltf_load_buffers(&options, data, file.c_str()) != cgltf_result_success)
            {
                Log::Error("Failed to load glTF '{}'", file);
                if (data) cgltf_free(data);
                return false;
            }

            // Skinned files: import the first skin and the meshes deformed by it (in mesh space, as glTF requires).
            const cgltf_skin* skin = data->skins_count > 0 ? &data->skins[0] : nullptr;
            std::unordered_map<const cgltf_node*, int> jointOf;
            std::vector<int> identity;
            if (skin)
            {
                out.SkeletonData = ReadSkeleton(*skin, jointOf);
                identity.resize(skin->joints_count);
                for (size_t i = 0; i < identity.size(); ++i) identity[i] = (int)i;
            }

            bool needNormals = false;
            size_t skipped = 0;
            for (cgltf_size n = 0; n < data->nodes_count; ++n)
            {
                const cgltf_node* node = &data->nodes[n];
                if (!node->mesh) continue;
                if (skin && node->skin != skin)
                {
                    ++skipped;
                    continue;
                }
                glm::mat4 transform = skin ? glm::mat4(1.0f) : NodeWorld(node);
                for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p)
                {
                    const cgltf_primitive& prim = node->mesh->primitives[p];
                    if (prim.type != cgltf_primitive_type_triangles) continue;
                    SubMesh sub;
                    sub.IndexOffset = (uint32_t)out.Indices.size();
                    sub.MaterialSlot = prim.material ? (int32_t)(prim.material - data->materials) : -1;
                    ReadPrimitive(prim, transform, out, needNormals, skin ? &identity : nullptr);
                    sub.IndexCount = (uint32_t)out.Indices.size() - sub.IndexOffset;
                    // Consecutive primitives with the same material share one draw.
                    if (!out.SubMeshes.empty() && out.SubMeshes.back().MaterialSlot == sub.MaterialSlot &&
                        out.SubMeshes.back().IndexOffset + out.SubMeshes.back().IndexCount == sub.IndexOffset)
                        out.SubMeshes.back().IndexCount += sub.IndexCount;
                    else if (sub.IndexCount > 0)
                        out.SubMeshes.push_back(sub);
                }
            }
            if (skin) ReadAnimations(data, jointOf, out);
            ReadMaterials(data, options, path, out);
            cgltf_free(data);

            if (skipped) Log::Warn("glTF '{}': {} unskinned mesh node(s) ignored in a skinned file", path.filename().string(), skipped);
            if (out.Vertices.empty())
            {
                Log::Error("glTF '{}' contains no triangle meshes", file);
                return false;
            }
            if (needNormals) ComputeNormals(out.Vertices, out.Indices);
            return true;
        }

        bool LoadObj(const fs::path& path, MeshData& out)
        {
            std::ifstream in(path);
            if (!in)
            {
                Log::Error("Failed to open OBJ '{}'", path.string());
                return false;
            }
            std::vector<glm::vec3> positions, normals;
            std::vector<glm::vec2> uvs;
            std::unordered_map<std::string, uint32_t> cache;
            bool hasNormals = false;
            auto& vertices = out.Vertices;
            auto& indices = out.Indices;

            auto resolve = [&](const std::string& token) -> uint32_t {
                if (auto it = cache.find(token); it != cache.end()) return it->second;
                int vi = 0, ti = 0, ni = 0;
                std::sscanf(token.c_str(), "%d/%d/%d", &vi, &ti, &ni);
                if (token.find("//") != std::string::npos) std::sscanf(token.c_str(), "%d//%d", &vi, &ni);
                auto fix = [](int i, size_t n) { return i < 0 ? (int)n + i : i - 1; };
                Vertex v{};
                if (vi) v.Position = positions[fix(vi, positions.size())];
                if (ti) v.UV = uvs[fix(ti, uvs.size())];
                if (ni) { v.Normal = normals[fix(ni, normals.size())]; hasNormals = true; }
                uint32_t index = (uint32_t)vertices.size();
                vertices.push_back(v);
                cache[token] = index;
                return index;
            };

            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v") { glm::vec3 p; ss >> p.x >> p.y >> p.z; positions.push_back(p); }
                else if (tag == "vt") { glm::vec2 t; ss >> t.x >> t.y; uvs.push_back({ t.x, 1.0f - t.y }); }
                else if (tag == "vn") { glm::vec3 n; ss >> n.x >> n.y >> n.z; normals.push_back(n); }
                else if (tag == "f")
                {
                    std::vector<uint32_t> face;
                    std::string tok;
                    while (ss >> tok) face.push_back(resolve(tok));
                    for (size_t i = 1; i + 1 < face.size(); ++i)
                    {
                        indices.push_back(face[0]);
                        indices.push_back(face[i]);
                        indices.push_back(face[i + 1]);
                    }
                }
            }
            if (vertices.empty()) return false;
            if (!hasNormals) ComputeNormals(vertices, indices);
            return true;
        }
    }

    bool MeshData::LoadFromFile(const fs::path& path, MeshData& out)
    {
        out = MeshData();
        out.Name = path.filename().string();
        auto ext = path.extension().string();
        for (auto& c : ext) c = (char)std::tolower(c);
        bool ok = false;
        if (ext == ".gltf" || ext == ".glb") ok = LoadGltf(path, out);
        else if (ext == ".obj") ok = LoadObj(path, out);
        else Log::Error("Unsupported mesh format '{}'", ext);
        if (ok && out.SubMeshes.empty()) out.SubMeshes.push_back({ 0, (uint32_t)out.Indices.size(), -1 });
        return ok;
    }

    Mesh::Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, const std::string& name)
        : m_Name(name)
    {
        Upload(vertices, indices);
        m_SubMeshes.push_back({ 0, m_IndexCount, -1 });
    }

    Mesh::Mesh(MeshData&& data)
        : m_Name(std::move(data.Name)), m_Skeleton(std::move(data.SkeletonData)), m_Clips(std::move(data.Clips)), m_SubMeshes(std::move(data.SubMeshes))
    {
        Upload(data.Vertices, data.Indices);
        if (m_SubMeshes.empty()) m_SubMeshes.push_back({ 0, m_IndexCount, -1 });
        if (m_Skeleton && data.Skin.size() == data.Vertices.size())
        {
            m_SkinBuffer = Buffer::CreateWithData(data.Skin.data(), data.Skin.size() * sizeof(SkinVertex),
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "SkinData");
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = m_SkinBuffer.GetHandle();
            m_SkinAddress = vkGetBufferDeviceAddress(RenderDevice::Get().GetDevice(), &info);
        }
        else
            m_Skeleton.reset();
    }

    void Mesh::Upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
    {
        DIM_ASSERT(!vertices.empty() && !indices.empty(), "Mesh needs vertices and indices");
        m_IndexCount = (uint32_t)indices.size();
        m_VertexCount = (uint32_t)vertices.size();
        m_Bounds.Min = m_Bounds.Max = vertices[0].Position;
        for (auto& v : vertices)
        {
            m_Bounds.Min = glm::min(m_Bounds.Min, v.Position);
            m_Bounds.Max = glm::max(m_Bounds.Max, v.Position);
        }
        m_VertexBuffer = Buffer::CreateWithData(vertices.data(), vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_Name.c_str());
        m_IndexBuffer = Buffer::CreateWithData(indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_Name.c_str());
    }

    void Mesh::Bind(VkCommandBuffer cmd) const
    {
        VkBuffer vb = m_VertexBuffer.GetHandle();
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, m_IndexBuffer.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
    }

    MeshData MeshData::FromArrays(std::vector<Vertex> vertices, std::vector<uint32_t> indices, const std::string& name)
    {
        MeshData d;
        d.Name = name;
        d.Vertices = std::move(vertices);
        d.Indices = std::move(indices);
        d.SubMeshes.push_back({ 0, (uint32_t)d.Indices.size(), -1 });
        return d;
    }

    Ref<Mesh> Mesh::LoadFromFile(const fs::path& path)
    {
        MeshData data;
        if (!MeshData::LoadFromFile(path, data)) return nullptr;
        return MakeRef<Mesh>(std::move(data));
    }

    MeshData MeshData::Cube()
    {
        struct Face { glm::vec3 n, u, v; };
        const Face faces[] = {
            { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        };
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        for (auto& f : faces)
        {
            uint32_t base = (uint32_t)v.size();
            glm::vec3 c = f.n * 0.5f, u = f.u * 0.5f, w = f.v * 0.5f;
            v.push_back({ c - u - w, f.n, { 0, 1 } });
            v.push_back({ c + u - w, f.n, { 1, 1 } });
            v.push_back({ c + u + w, f.n, { 1, 0 } });
            v.push_back({ c - u + w, f.n, { 0, 0 } });
            idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
        FixWinding(v, idx);
        return FromArrays(std::move(v), std::move(idx), "Cube");
    }

    MeshData MeshData::Sphere(uint32_t segments, uint32_t rings)
    {
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        for (uint32_t r = 0; r <= rings; ++r)
        {
            float theta = glm::pi<float>() * (float)r / (float)rings;
            for (uint32_t s = 0; s <= segments; ++s)
            {
                float phi = glm::two_pi<float>() * (float)s / (float)segments;
                glm::vec3 n{ std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi) };
                v.push_back({ n * 0.5f, n, { (float)s / segments, (float)r / rings } });
            }
        }
        for (uint32_t r = 0; r < rings; ++r)
            for (uint32_t s = 0; s < segments; ++s)
            {
                uint32_t a = r * (segments + 1) + s, b = a + segments + 1;
                if (r != 0) idx.insert(idx.end(), { a, b, a + 1 });
                if (r != rings - 1) idx.insert(idx.end(), { a + 1, b, b + 1 });
            }
        FixWinding(v, idx);
        return FromArrays(std::move(v), std::move(idx), "Sphere");
    }

    MeshData MeshData::Plane()
    {
        constexpr int kDiv = 10;
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        for (int z = 0; z <= kDiv; ++z)
            for (int x = 0; x <= kDiv; ++x)
                v.push_back({ { (float)x - kDiv * 0.5f, 0.0f, (float)z - kDiv * 0.5f }, { 0, 1, 0 }, { (float)x / kDiv, (float)z / kDiv } });
        for (int z = 0; z < kDiv; ++z)
            for (int x = 0; x < kDiv; ++x)
            {
                uint32_t a = z * (kDiv + 1) + x, b = a + kDiv + 1;
                idx.insert(idx.end(), { a, b, a + 1, a + 1, b, b + 1 });
            }
        FixWinding(v, idx);
        return FromArrays(std::move(v), std::move(idx), "Plane");
    }

    MeshData MeshData::Quad()
    {
        std::vector<Vertex> v = {
            { { -0.5f, -0.5f, 0 }, { 0, 0, 1 }, { 0, 1 } }, { { 0.5f, -0.5f, 0 }, { 0, 0, 1 }, { 1, 1 } },
            { { 0.5f, 0.5f, 0 }, { 0, 0, 1 }, { 1, 0 } },   { { -0.5f, 0.5f, 0 }, { 0, 0, 1 }, { 0, 0 } },
        };
        std::vector<uint32_t> idx = { 0, 1, 2, 0, 2, 3 };
        FixWinding(v, idx);
        return FromArrays(std::move(v), std::move(idx), "Quad");
    }

    MeshData MeshData::Cylinder(uint32_t segments)
    {
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        const float r = 0.5f, h = 1.0f;
        for (uint32_t s = 0; s <= segments; ++s) // side
        {
            float phi = glm::two_pi<float>() * (float)s / (float)segments;
            glm::vec3 n{ std::cos(phi), 0, std::sin(phi) };
            float u = (float)s / segments;
            v.push_back({ n * r + glm::vec3(0, -h, 0), n, { u, 1 } });
            v.push_back({ n * r + glm::vec3(0, h, 0), n, { u, 0 } });
        }
        for (uint32_t s = 0; s < segments; ++s)
        {
            uint32_t a = s * 2;
            idx.insert(idx.end(), { a, a + 1, a + 2, a + 2, a + 1, a + 3 });
        }
        for (float y : { -h, h }) // caps
        {
            glm::vec3 n{ 0, y > 0 ? 1.0f : -1.0f, 0 };
            uint32_t center = (uint32_t)v.size();
            v.push_back({ { 0, y, 0 }, n, { 0.5f, 0.5f } });
            for (uint32_t s = 0; s <= segments; ++s)
            {
                float phi = glm::two_pi<float>() * (float)s / (float)segments;
                v.push_back({ { std::cos(phi) * r, y, std::sin(phi) * r }, n, { 0.5f + std::cos(phi) * 0.5f, 0.5f + std::sin(phi) * 0.5f } });
            }
            for (uint32_t s = 0; s < segments; ++s) idx.insert(idx.end(), { center, center + 1 + s, center + 2 + s });
        }
        FixWinding(v, idx);
        return FromArrays(std::move(v), std::move(idx), "Cylinder");
    }

    Ref<Mesh> Mesh::CreateCube() { return MakeRef<Mesh>(MeshData::Cube()); }

    Ref<Mesh> Mesh::CreateSphere(uint32_t segments, uint32_t rings) { return MakeRef<Mesh>(MeshData::Sphere(segments, rings)); }

    Ref<Mesh> Mesh::CreatePlane() { return MakeRef<Mesh>(MeshData::Plane()); }

    Ref<Mesh> Mesh::CreateQuad() { return MakeRef<Mesh>(MeshData::Quad()); }

    Ref<Mesh> Mesh::CreateCylinder(uint32_t segments) { return MakeRef<Mesh>(MeshData::Cylinder(segments)); }
}
