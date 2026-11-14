#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Asset/ImportCache.h"
#include "Dimension/Modeling/EditableMesh.h"
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

        // A primitive's morph targets (its vertices from `base` on) as blend shapes, matched across primitives by name
        // (the mesh's target names, else "Shape N"). `shapeOf` gets the blend shape of each target.
        void ReadTargets(const cgltf_primitive& prim, const cgltf_mesh& mesh, const glm::mat4& transform, size_t base, MeshData& out,
                         std::vector<int>& shapeOf)
        {
            glm::mat3 linear(transform), normalMatrix = glm::transpose(glm::inverse(linear));
            shapeOf.assign(prim.targets_count, -1);
            for (cgltf_size t = 0; t < prim.targets_count; ++t)
            {
                std::string name = t < mesh.target_names_count && mesh.target_names[t] && *mesh.target_names[t] ? mesh.target_names[t]
                                                                                                                 : "Shape " + std::to_string(t);
                int index = out.FindBlendShape(name);
                if (index < 0)
                {
                    BlendShape shape;
                    shape.Name = name;
                    shape.DefaultWeight = t < mesh.weights_count ? mesh.weights[t] : 0.0f;
                    out.BlendShapes.push_back(std::move(shape));
                    index = (int)out.BlendShapes.size() - 1;
                }
                shapeOf[t] = index;
                BlendShape& shape = out.BlendShapes[index];
                shape.Positions.resize(out.Vertices.size(), glm::vec3(0.0f));
                shape.Normals.resize(out.Vertices.size(), glm::vec3(0.0f));
                const cgltf_morph_target& target = prim.targets[t];
                for (cgltf_size a = 0; a < target.attributes_count; ++a)
                {
                    const cgltf_attribute& attr = target.attributes[a];
                    bool position = attr.type == cgltf_attribute_type_position, normal = attr.type == cgltf_attribute_type_normal;
                    if (!position && !normal) continue;
                    for (cgltf_size i = 0; i < attr.data->count && base + i < out.Vertices.size(); ++i)
                    {
                        glm::vec3 d(0.0f);
                        cgltf_accessor_read_float(attr.data, i, glm::value_ptr(d), 3);
                        if (position) shape.Positions[base + i] = linear * d;
                        else shape.Normals[base + i] = normalMatrix * d;
                    }
                }
            }
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

        void ReadAnimations(const cgltf_data* data, const std::unordered_map<const cgltf_node*, int>& jointOf,
                            const std::unordered_map<const cgltf_mesh*, std::vector<int>>& meshShapes, MeshData& out)
        {
            for (cgltf_size a = 0; a < data->animations_count; ++a)
            {
                const cgltf_animation& anim = data->animations[a];
                AnimationClip clip;
                clip.Name = anim.name && *anim.name ? anim.name : ("Clip" + std::to_string(a));
                for (cgltf_size c = 0; c < anim.channels_count; ++c)
                {
                    const cgltf_animation_channel& ch = anim.channels[c];
                    if (ch.target_path == cgltf_animation_path_type_weights)
                    {
                        // Morph target weights of a mesh node: a channel per blend shape (the output holds every
                        // target's weight per key, in target order).
                        auto shapes = ch.target_node && ch.target_node->mesh ? meshShapes.find(ch.target_node->mesh) : meshShapes.end();
                        if (shapes == meshShapes.end() || !ch.sampler || shapes->second.empty()) continue;
                        const cgltf_animation_sampler& s = *ch.sampler;
                        bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline;
                        size_t targets = shapes->second.size();
                        for (size_t t = 0; t < targets; ++t)
                        {
                            int shape = shapes->second[t];
                            bool seen = shape < 0;
                            for (const AnimationChannel& w : clip.WeightChannels) seen |= w.Joint == shape;
                            if (seen) continue;
                            AnimationChannel w;
                            w.Joint = shape;
                            w.Path = AnimationPath::Translation; // (plain values: mixed, not turned)
                            w.Interpolation = s.interpolation == cgltf_interpolation_type_step ? AnimationInterpolation::Step
                                              : cubic ? AnimationInterpolation::CubicSpline : AnimationInterpolation::Linear;
                            auto read = [&](size_t element) {
                                float v = 0.0f;
                                cgltf_accessor_read_float(s.output, element, &v, 1);
                                return glm::vec4(v, 0.0f, 0.0f, 0.0f);
                            };
                            for (cgltf_size k = 0; k < s.input->count; ++k)
                            {
                                float time = 0.0f;
                                cgltf_accessor_read_float(s.input, k, &time, 1);
                                w.Times.push_back(time);
                                w.Values.push_back(read(cubic ? (k * 3 + 1) * targets + t : k * targets + t));
                                if (cubic)
                                {
                                    w.InTangents.push_back(read(k * 3 * targets + t));
                                    w.OutTangents.push_back(read((k * 3 + 2) * targets + t));
                                }
                                clip.Duration = std::max(clip.Duration, time);
                            }
                            clip.WeightChannels.push_back(std::move(w));
                        }
                        continue;
                    }
                    auto it = jointOf.find(ch.target_node);
                    if (it == jointOf.end() || !ch.sampler) continue;
                    AnimationChannel channel;
                    channel.Joint = it->second;
                    if (ch.target_path == cgltf_animation_path_type_translation) channel.Path = AnimationPath::Translation;
                    else if (ch.target_path == cgltf_animation_path_type_rotation) channel.Path = AnimationPath::Rotation;
                    else if (ch.target_path == cgltf_animation_path_type_scale) channel.Path = AnimationPath::Scale;
                    else continue; // morph weights are not supported
                    const cgltf_animation_sampler& s = *ch.sampler;
                    bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline; // (in tangent, value, out tangent) per key
                    channel.Interpolation = s.interpolation == cgltf_interpolation_type_step ? AnimationInterpolation::Step
                                            : cubic ? AnimationInterpolation::CubicSpline : AnimationInterpolation::Linear;
                    int comps = channel.Path == AnimationPath::Rotation ? 4 : 3;
                    for (cgltf_size k = 0; k < s.input->count; ++k)
                    {
                        float t = 0.0f;
                        cgltf_accessor_read_float(s.input, k, &t, 1);
                        glm::vec4 v(0.0f, 0.0f, 0.0f, 1.0f);
                        cgltf_accessor_read_float(s.output, cubic ? k * 3 + 1 : k, glm::value_ptr(v), comps);
                        channel.Times.push_back(t);
                        channel.Values.push_back(v);
                        if (cubic)
                        {
                            glm::vec4 inTangent(0.0f), outTangent(0.0f);
                            cgltf_accessor_read_float(s.output, k * 3, glm::value_ptr(inTangent), comps);
                            cgltf_accessor_read_float(s.output, k * 3 + 2, glm::value_ptr(outTangent), comps);
                            channel.InTangents.push_back(inTangent);
                            channel.OutTangents.push_back(outTangent);
                        }
                        clip.Duration = std::max(clip.Duration, t);
                    }
                    clip.Channels.push_back(std::move(channel));
                }
                if (!clip.Channels.empty() || !clip.WeightChannels.empty()) out.Clips.push_back(std::move(clip));
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
                    im.File = (path.parent_path() / Utf8Path(uri)).lexically_normal();
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
            std::unordered_map<const cgltf_mesh*, std::vector<int>> meshShapes; // blend shape of each morph target
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
                    size_t firstVertex = out.Vertices.size();
                    ReadPrimitive(prim, transform, out, needNormals, skin ? &identity : nullptr);
                    if (prim.targets_count > 0)
                    {
                        std::vector<int> shapeOf;
                        ReadTargets(prim, *node->mesh, transform, firstVertex, out, shapeOf);
                        meshShapes.try_emplace(node->mesh, shapeOf);
                    }
                    sub.IndexCount = (uint32_t)out.Indices.size() - sub.IndexOffset;
                    // Consecutive primitives with the same material share one draw.
                    if (!out.SubMeshes.empty() && out.SubMeshes.back().MaterialSlot == sub.MaterialSlot &&
                        out.SubMeshes.back().IndexOffset + out.SubMeshes.back().IndexCount == sub.IndexOffset)
                        out.SubMeshes.back().IndexCount += sub.IndexCount;
                    else if (sub.IndexCount > 0)
                        out.SubMeshes.push_back(sub);
                }
            }
            for (BlendShape& shape : out.BlendShapes) // (zero for the vertices of meshes without it)
            {
                shape.Positions.resize(out.Vertices.size(), glm::vec3(0.0f));
                shape.Normals.resize(out.Vertices.size(), glm::vec3(0.0f));
            }
            if (skin) ReadAnimations(data, jointOf, meshShapes, out);
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

        // An OBJ material library (.mtl): diffuse color (Kd) and map, opacity (d, or Tr = 1 - d), shininess (Ns, as
        // roughness), emission (Ke) and normal map (map_Bump, bump, norm), into `out`'s images; returns name -> slot.
        std::unordered_map<std::string, int32_t> LoadMtl(const fs::path& file, MeshData& out)
        {
            std::unordered_map<std::string, int32_t> slots;
            std::ifstream in(file);
            if (!in)
            {
                Log::Warn("OBJ material library '{}' not found", file.string());
                return slots;
            }
            auto image = [&](std::istringstream& ss) -> int32_t {
                std::string rest, token;
                while (ss >> token)
                    if (token[0] != '-') rest = token; // options (-bm 1 ...) come before the file name
                    else ss >> std::ws;
                if (rest.empty()) return -1;
                ImportedImage im;
                im.Name = Utf8Path(rest).stem().string();
                im.File = (file.parent_path() / Utf8Path(rest)).lexically_normal();
                out.Images.push_back(std::move(im));
                return (int32_t)out.Images.size() - 1;
            };
            ImportedMaterial* m = nullptr;
            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "newmtl")
                {
                    std::string name;
                    std::getline(ss >> std::ws, name);
                    slots[name] = (int32_t)out.Materials.size();
                    out.Materials.push_back({});
                    m = &out.Materials.back();
                    m->Name = name;
                    m->Values.Metallic = 0.0f;
                }
                else if (!m) continue;
                else if (tag == "Kd") { glm::vec3 c(1.0f); ss >> c.r >> c.g >> c.b; m->Values.BaseColor = glm::vec4(c, m->Values.BaseColor.a); }
                else if (tag == "d" || tag == "Tr")
                {
                    float d = 1.0f;
                    ss >> d;
                    float alpha = std::clamp(tag == "Tr" ? 1.0f - d : d, 0.0f, 1.0f);
                    m->Values.BaseColor.a = alpha;
                    if (alpha < 0.999f) m->Values.BlendMode = MaterialBlendMode::Transparent;
                }
                else if (tag == "Ns") { float ns = 0.0f; ss >> ns; m->Values.Roughness = std::clamp(std::sqrt(2.0f / (std::max(ns, 0.0f) + 2.0f)), 0.05f, 1.0f); }
                else if (tag == "Ke") { glm::vec3 c(0.0f); ss >> c.r >> c.g >> c.b; m->Values.EmissionColor = c; }
                else if (tag == "map_Kd") m->BaseColorImage = image(ss);
                else if (tag == "map_Bump" || tag == "bump" || tag == "norm") m->NormalImage = image(ss);
                else if (tag == "map_Ke") m->EmissionImage = image(ss);
            }
            return slots;
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
            // Triangles per material (usemtl), joined into one sub-mesh each at the end.
            std::unordered_map<std::string, int32_t> library;
            std::vector<std::pair<int32_t, std::vector<uint32_t>>> groups{ { -1, {} } };
            size_t current = 0;

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
                    auto& indices = groups[current].second;
                    for (size_t i = 1; i + 1 < face.size(); ++i)
                    {
                        indices.push_back(face[0]);
                        indices.push_back(face[i]);
                        indices.push_back(face[i + 1]);
                    }
                }
                else if (tag == "mtllib")
                {
                    std::string file;
                    std::getline(ss >> std::ws, file);
                    while (!file.empty() && (file.back() == '\r' || file.back() == ' ')) file.pop_back();
                    for (auto& [name, slot] : LoadMtl(path.parent_path() / Utf8Path(file), out)) library[name] = slot;
                }
                else if (tag == "usemtl")
                {
                    std::string name;
                    std::getline(ss >> std::ws, name);
                    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                    auto it = library.find(name);
                    int32_t slot = it != library.end() ? it->second : -1;
                    current = groups.size();
                    for (size_t g = 0; g < groups.size(); ++g)
                        if (groups[g].first == slot) current = g;
                    if (current == groups.size()) groups.push_back({ slot, {} });
                }
            }
            if (vertices.empty()) return false;
            for (auto& [slot, indices] : groups)
            {
                if (indices.empty()) continue;
                out.SubMeshes.push_back({ (uint32_t)out.Indices.size(), (uint32_t)indices.size(), slot });
                out.Indices.insert(out.Indices.end(), indices.begin(), indices.end());
            }
            if (out.SubMeshes.size() == 1 && out.SubMeshes[0].MaterialSlot < 0) out.SubMeshes.clear(); // one plain part
            if (!hasNormals) ComputeNormals(vertices, out.Indices);
            return true;
        }
    }

    std::string MeshImportSettings::Key() const
    {
        if (IsDefault()) return {};
        char key[64];
        std::snprintf(key, sizeof(key), "s%g a%d m%d", Scale, ImportAnimations ? 1 : 0, ImportMaterials ? 1 : 0);
        std::string result = key;
        if (ReduceKeyframes)
        {
            std::snprintf(key, sizeof(key), " r%g p%g", RotationError, PositionError);
            result += key;
        }
        for (const auto& name : ExcludedClips) result += " -" + name;
        return result;
    }

    void MeshData::ApplyImportSettings(const MeshImportSettings& settings)
    {
        if (!settings.ImportAnimations) Clips.clear();
        std::erase_if(Clips, [&](const AnimationClip& c) {
            return std::find(settings.ExcludedClips.begin(), settings.ExcludedClips.end(), c.Name) != settings.ExcludedClips.end();
        });
        if (!settings.ImportMaterials)
        {
            Materials.clear();
            Images.clear();
            for (auto& sub : SubMeshes) sub.MaterialSlot = -1;
        }
        float s = settings.Scale;
        if (s == 1.0f || s <= 0.0f)
        {
            ReduceClipKeys(settings);
            return;
        }
        // A uniform scale of the whole model: positions and every translation (joint offsets, the bind pose's
        // translation, animated translations, the root transform) grow by s; rotations and scales stay.
        for (auto& v : Vertices) v.Position *= s;
        for (auto& shape : BlendShapes)
            for (auto& d : shape.Positions) d *= s;
        if (SkeletonData)
        {
            SkeletonData->RootTransform[3] = glm::vec4(glm::vec3(SkeletonData->RootTransform[3]) * s, 1.0f);
            for (auto& j : SkeletonData->Joints)
            {
                j.Translation *= s;
                j.InverseBind[3] = glm::vec4(glm::vec3(j.InverseBind[3]) * s, 1.0f);
            }
        }
        for (auto& clip : Clips)
            for (auto& ch : clip.Channels)
                if (ch.Path == AnimationPath::Translation)
                    for (auto& value : ch.Values) value = glm::vec4(glm::vec3(value) * s, value.w);
        ReduceClipKeys(settings);
    }

    void MeshData::ReduceClipKeys(const MeshImportSettings& settings)
    {
        if (!settings.ReduceKeyframes) return;
        int removed = 0, total = 0;
        for (auto& clip : Clips)
        {
            for (const auto& ch : clip.Channels) total += (int)ch.Times.size();
            removed += Animation::ReduceKeys(clip, settings.RotationError, settings.PositionError);
        }
        if (removed > 0) Log::Info("'{}': keyframe reduction left out {} of {} keys", Name, removed, total);
    }

    bool LoadFbx(const fs::path& path, MeshData& out); // (FbxImport.cpp)

    bool MeshData::LoadFromFile(const fs::path& path, MeshData& out, const MeshImportSettings& settings)
    {
        out = MeshData();
        auto ext = path.extension().string();
        for (auto& c : ext) c = (char)std::tolower(c);
        if (ext == ".dmodel")
        {
            // Models made in the editor: the editable mesh, triangulated (fast enough not to need the import cache).
            Modeling::EditableMesh model;
            if (!Modeling::EditableMesh::Load(path, model)) return false;
            out = model.ToMeshData();
            if (out.SubMeshes.empty()) out.SubMeshes.push_back({ 0, (uint32_t)out.Indices.size(), -1 });
            out.ApplyImportSettings(settings);
            return true;
        }
        if (ImportCache::LoadMesh(path, settings.Key(), out)) return true;
        out.Name = path.filename().string();
        bool ok = false;
        if (ext == ".gltf" || ext == ".glb") ok = LoadGltf(path, out);
        else if (ext == ".obj") ok = LoadObj(path, out);
        else if (ext == ".fbx") ok = LoadFbx(path, out);
        else Log::Error("Unsupported mesh format '{}'", ext);
        if (ok && out.SubMeshes.empty()) out.SubMeshes.push_back({ 0, (uint32_t)out.Indices.size(), -1 });
        if (ok)
        {
            out.GenerateTangents();
            out.ApplyImportSettings(settings);
            ImportCache::StoreMesh(path, settings.Key(), out);
        }
        return ok;
    }

    int MeshData::FindBlendShape(const std::string& name) const
    {
        for (size_t i = 0; i < BlendShapes.size(); ++i)
            if (BlendShapes[i].Name == name) return (int)i;
        return -1;
    }

    AABB MeshData::ApplyBlendShapes(const float* weights, size_t count, Vertex* out) const
    {
        // Only the shapes with weight count.
        std::vector<std::pair<const BlendShape*, float>> active;
        for (size_t s = 0; s < count && s < BlendShapes.size(); ++s)
            if (weights[s] != 0.0f && BlendShapes[s].Positions.size() == Vertices.size()) active.push_back({ &BlendShapes[s], weights[s] });
        AABB bounds;
        for (size_t i = 0; i < Vertices.size(); ++i)
        {
            Vertex v = Vertices[i];
            glm::vec3 n = v.Normal;
            for (const auto& [shape, w] : active)
            {
                v.Position += shape->Positions[i] * w;
                n += shape->Normals[i] * w;
            }
            if (!active.empty() && glm::dot(n, n) > 1e-12f)
            {
                v.Normal = glm::normalize(n);
                glm::vec3 t = glm::vec3(v.Tangent) - v.Normal * glm::dot(v.Normal, glm::vec3(v.Tangent));
                if (v.Tangent.w != 0.0f && glm::dot(t, t) > 1e-12f) v.Tangent = glm::vec4(glm::normalize(t), v.Tangent.w);
            }
            out[i] = v;
            if (i == 0) bounds.Min = bounds.Max = v.Position;
            bounds.Min = glm::min(bounds.Min, v.Position);
            bounds.Max = glm::max(bounds.Max, v.Position);
        }
        return bounds;
    }

    void MeshData::GenerateTangents()
    {
        size_t original = Vertices.size();
        bool missing = false;
        for (const Vertex& v : Vertices) missing |= v.Tangent.w == 0.0f;
        if (!missing || Indices.size() < 3) return;

        // Accumulated tangent per vertex, kept apart by the handedness of the triangles adding to it.
        std::vector<glm::vec3> accum[2] = { std::vector<glm::vec3>(original, glm::vec3(0.0f)), std::vector<glm::vec3>(original, glm::vec3(0.0f)) };
        std::vector<int8_t> faceSign(Indices.size() / 3, 0);
        for (size_t f = 0; f + 2 < Indices.size(); f += 3)
        {
            uint32_t i[3] = { Indices[f], Indices[f + 1], Indices[f + 2] };
            if (Vertices[i[0]].Tangent.w != 0.0f && Vertices[i[1]].Tangent.w != 0.0f && Vertices[i[2]].Tangent.w != 0.0f) continue;
            const Vertex &a = Vertices[i[0]], &b = Vertices[i[1]], &c = Vertices[i[2]];
            glm::vec3 e1 = b.Position - a.Position, e2 = c.Position - a.Position;
            glm::vec2 d1 = b.UV - a.UV, d2 = c.UV - a.UV;
            float det = d1.x * d2.y - d2.x * d1.y;
            if (std::abs(det) < 1e-12f) continue;
            glm::vec3 t = (e1 * d2.y - e2 * d1.y) / det;
            glm::vec3 bt = (e2 * d1.x - e1 * d2.x) / det;
            glm::vec3 n = glm::cross(e1, e2);
            if (glm::dot(t, t) < 1e-20f || glm::dot(n, n) < 1e-20f) continue;
            // glTF convention: cross(N, T) * w points up the image, i.e. against increasing V (which runs downwards).
            // Side 0 (w = +1) is the usual layout, side 1 (w = -1) has mirrored UVs.
            int side = glm::dot(glm::cross(n, t), bt) < 0.0f ? 0 : 1;
            faceSign[f / 3] = side ? -1 : 1;
            t = glm::normalize(t);
            for (int k = 0; k < 3; ++k)
            {
                const glm::vec3& p = Vertices[i[k]].Position;
                glm::vec3 u = Vertices[i[(k + 1) % 3]].Position - p, w = Vertices[i[(k + 2) % 3]].Position - p;
                float lu = glm::length(u), lw = glm::length(w);
                if (lu < 1e-12f || lw < 1e-12f) continue;
                float angle = std::acos(std::clamp(glm::dot(u, w) / (lu * lw), -1.0f, 1.0f));
                accum[side][i[k]] += t * angle;
            }
        }

        // Vertices used by both handedness sides are split: the mirrored triangles get a copy.
        std::vector<uint32_t> mirrorCopy(original, UINT32_MAX);
        for (size_t f = 0; f + 2 < Indices.size(); f += 3)
        {
            if (faceSign[f / 3] != -1) continue;
            for (int k = 0; k < 3; ++k)
            {
                uint32_t& index = Indices[f + k];
                if (index >= original || Vertices[index].Tangent.w != 0.0f || glm::dot(accum[0][index], accum[0][index]) == 0.0f) continue;
                if (mirrorCopy[index] == UINT32_MAX)
                {
                    mirrorCopy[index] = (uint32_t)Vertices.size();
                    Vertices.push_back(Vertices[index]);
                    if (!Skin.empty()) Skin.push_back(Skin[index]);
                    for (BlendShape& shape : BlendShapes)
                    {
                        glm::vec3 p = shape.Positions[index], n = shape.Normals[index];
                        shape.Positions.push_back(p);
                        shape.Normals.push_back(n);
                    }
                }
                index = mirrorCopy[index];
            }
        }

        auto finish = [&](Vertex& v, const glm::vec3& sum, float handedness) {
            glm::vec3 t = sum - v.Normal * glm::dot(v.Normal, sum);
            if (glm::dot(t, t) > 1e-16f) v.Tangent = glm::vec4(glm::normalize(t), handedness);
        };
        for (size_t v = 0; v < original; ++v)
        {
            if (Vertices[v].Tangent.w != 0.0f) continue;
            bool right = glm::dot(accum[0][v], accum[0][v]) > 0.0f;
            if (right) finish(Vertices[v], accum[0][v], 1.0f);
            else finish(Vertices[v], accum[1][v], -1.0f);
            if (mirrorCopy[v] != UINT32_MAX) finish(Vertices[mirrorCopy[v]], accum[1][v], -1.0f);
        }
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
        m_BlendShapeCount = (uint32_t)data.BlendShapes.size();
        if (m_BlendShapeCount > 0)
        {
            // The shapes' deltas for the GPU, and each one's reach (for the moved mesh's bounds).
            size_t v = data.Vertices.size();
            std::vector<glm::vec4> deltas(v * 2 * m_BlendShapeCount, glm::vec4(0.0f));
            m_BlendReach.assign(m_BlendShapeCount, glm::vec3(0.0f));
            for (size_t s = 0; s < m_BlendShapeCount; ++s)
            {
                const BlendShape& shape = data.BlendShapes[s];
                for (size_t i = 0; i < v && i < shape.Positions.size() && i < shape.Normals.size(); ++i)
                {
                    deltas[(s * v + i) * 2] = glm::vec4(shape.Positions[i], 0.0f);
                    deltas[(s * v + i) * 2 + 1] = glm::vec4(shape.Normals[i], 0.0f);
                    m_BlendReach[s] = glm::max(m_BlendReach[s], glm::abs(shape.Positions[i]));
                }
            }
            m_BlendBuffer = Buffer::CreateWithData(deltas.data(), deltas.size() * sizeof(glm::vec4),
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "BlendShapes");
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = m_BlendBuffer.GetHandle();
            m_BlendAddress = vkGetBufferDeviceAddress(RenderDevice::Get().GetDevice(), &info);
        }
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
        // (also read by the blend shape compute pass, by address)
        m_VertexBuffer = Buffer::CreateWithData(vertices.data(), vertices.size() * sizeof(Vertex),
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                                m_Name.c_str());
        VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        info.buffer = m_VertexBuffer.GetHandle();
        m_VertexAddress = vkGetBufferDeviceAddress(RenderDevice::Get().GetDevice(), &info);
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
        d.GenerateTangents();
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
