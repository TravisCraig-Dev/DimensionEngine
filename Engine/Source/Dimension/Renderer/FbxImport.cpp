// FBX models (ufbx): meshes, materials and textures, a skeleton with skin weights, animation takes and blend shapes,
// into the same MeshData as glTF. The scene is converted to the engine's space (meters, Y up, right-handed) in the
// geometry and animation themselves.
#include "Dimension/Renderer/Mesh.h"
#include "Dimension/Core/Log.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

#include <ufbx.h>

namespace Dimension
{
    namespace
    {
        glm::vec3 V3(ufbx_vec3 v) { return { (float)v.x, (float)v.y, (float)v.z }; }
        glm::quat Q(ufbx_quat q) { return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z); }
        glm::mat4 M4(const ufbx_matrix& m)
        {
            // ufbx: 3 columns of axes plus the translation column.
            return glm::mat4(glm::vec4(V3(m.cols[0]), 0.0f), glm::vec4(V3(m.cols[1]), 0.0f), glm::vec4(V3(m.cols[2]), 0.0f), glm::vec4(V3(m.cols[3]), 1.0f));
        }
        std::string Str(ufbx_string s) { return std::string(s.data, s.length); }

        // A vertex as the importer keys it: the FBX vertex it came from (for skin weights and blend shapes) and its
        // attributes, so corners that share all of them become one vertex.
        struct CornerKey
        {
            uint32_t FbxVertex;
            const ufbx_mesh* Mesh;
            Vertex V;
            bool operator==(const CornerKey& o) const { return FbxVertex == o.FbxVertex && Mesh == o.Mesh && std::memcmp(&V, &o.V, sizeof(Vertex)) == 0; }
        };
        struct CornerHash
        {
            size_t operator()(const CornerKey& k) const
            {
                size_t h = std::hash<uint32_t>()(k.FbxVertex) ^ (std::hash<const void*>()(k.Mesh) << 1);
                const uint32_t* w = reinterpret_cast<const uint32_t*>(&k.V);
                for (size_t i = 0; i < sizeof(Vertex) / 4; ++i) h = h * 1099511628211ull ^ w[i];
                return h;
            }
        };

        // A texture as an imported image (its file, or the bytes embedded in the FBX); -1 for none.
        int32_t Image(const ufbx_texture* texture, const fs::path& modelDir, MeshData& out, std::map<const ufbx_texture*, int32_t>& images)
        {
            if (!texture) return -1;
            if (auto it = images.find(texture); it != images.end()) return it->second;
            ImportedImage image;
            fs::path file = Utf8Path(Str(texture->filename));
            image.Name = file.stem().string();
            if (texture->content.size > 0)
            {
                const uint8_t* bytes = static_cast<const uint8_t*>(texture->content.data);
                image.Bytes.assign(bytes, bytes + texture->content.size);
            }
            else
            {
                // The path as written, else beside the model (exporters often keep the artist's absolute paths).
                fs::path relative = Utf8Path(Str(texture->relative_filename));
                std::error_code ec;
                if (!relative.empty() && fs::exists(modelDir / relative, ec)) image.File = (modelDir / relative).lexically_normal();
                else if (!file.empty() && fs::exists(file, ec)) image.File = file;
                else if (!file.empty() && fs::exists(modelDir / file.filename(), ec)) image.File = modelDir / file.filename();
                else
                {
                    Log::Warn("FBX texture '{}' not found", file.string());
                    return images[texture] = -1;
                }
            }
            out.Images.push_back(std::move(image));
            return images[texture] = (int32_t)out.Images.size() - 1;
        }

        void ReadMaterials(const ufbx_scene* scene, const fs::path& path, MeshData& out)
        {
            std::map<const ufbx_texture*, int32_t> images;
            for (size_t i = 0; i < scene->materials.count; ++i)
            {
                const ufbx_material* m = scene->materials.data[i];
                ImportedMaterial im;
                im.Name = Str(m->name);
                Material& v = im.Values;
                // PBR values (ufbx maps the classic FBX ones onto them too).
                const ufbx_material_pbr_maps& pbr = m->pbr;
                if (pbr.base_color.has_value)
                {
                    ufbx_vec4 c = pbr.base_color.value_vec4;
                    float factor = pbr.base_factor.has_value ? (float)pbr.base_factor.value_real : 1.0f;
                    v.BaseColor = glm::vec4((float)c.x * factor, (float)c.y * factor, (float)c.z * factor, 1.0f);
                }
                if (pbr.opacity.has_value && pbr.opacity.value_real < 0.999)
                {
                    v.BaseColor.a = std::clamp((float)pbr.opacity.value_real, 0.0f, 1.0f);
                    v.BlendMode = MaterialBlendMode::Transparent;
                }
                v.Metallic = pbr.metalness.has_value ? std::clamp((float)pbr.metalness.value_real, 0.0f, 1.0f) : 0.0f;
                if (pbr.roughness.has_value) v.Roughness = std::clamp((float)pbr.roughness.value_real, 0.05f, 1.0f);
                if (pbr.emission_color.has_value)
                {
                    float factor = pbr.emission_factor.has_value ? (float)pbr.emission_factor.value_real : 1.0f;
                    v.EmissionColor = V3(ufbx_vec3{ pbr.emission_color.value_vec4.x, pbr.emission_color.value_vec4.y, pbr.emission_color.value_vec4.z }) * factor;
                }
                fs::path dir = path.parent_path();
                im.BaseColorImage = Image(pbr.base_color.texture ? pbr.base_color.texture : m->fbx.diffuse_color.texture, dir, out, images);
                im.NormalImage = Image(pbr.normal_map.texture ? pbr.normal_map.texture : m->fbx.normal_map.texture, dir, out, images);
                im.EmissionImage = Image(pbr.emission_color.texture, dir, out, images);
                if (im.BaseColorImage >= 0 && !pbr.base_color.has_value) v.BaseColor = glm::vec4(1.0f);
                out.Materials.push_back(std::move(im));
            }
        }

        // The skeleton: every bone a skin uses, with the nodes between them and the scene root (a rig's own root
        // node), parents before children.
        Ref<Skeleton> ReadSkeleton(const ufbx_scene* scene, std::unordered_map<const ufbx_node*, int>& jointOf)
        {
            std::vector<const ufbx_node*> nodes;
            std::unordered_map<const ufbx_node*, glm::mat4> inverseBind;
            for (size_t s = 0; s < scene->skin_deformers.count; ++s)
                for (size_t c = 0; c < scene->skin_deformers.data[s]->clusters.count; ++c)
                {
                    const ufbx_skin_cluster* cluster = scene->skin_deformers.data[s]->clusters.data[c];
                    if (!cluster->bone_node) continue;
                    // World space at bind time (every vertex is put there, skinned and rigid alike).
                    inverseBind.try_emplace(cluster->bone_node, glm::inverse(M4(cluster->bind_to_world)));
                    for (const ufbx_node* n = cluster->bone_node; n && !n->is_root && !jointOf.contains(n); n = n->parent)
                    {
                        jointOf[n] = -1;
                        nodes.push_back(n);
                    }
                }
            if (nodes.empty()) return nullptr;
            // Scene order is parents first (ufbx sorts nodes by depth); index the joints in that order.
            std::sort(nodes.begin(), nodes.end(), [](const ufbx_node* a, const ufbx_node* b) { return a->typed_id < b->typed_id; });
            std::stable_sort(nodes.begin(), nodes.end(), [](const ufbx_node* a, const ufbx_node* b) { return a->node_depth < b->node_depth; });
            auto skeleton = MakeRef<Skeleton>();
            for (size_t i = 0; i < nodes.size(); ++i) jointOf[nodes[i]] = (int)i;
            // World matrices at bind time: bones from their skin, nodes added between them where they are. (A node's
            // own transform is the scene as saved, often some frame of a take, not the bind pose.)
            std::vector<glm::mat4> bindWorld;
            for (const ufbx_node* n : nodes)
            {
                auto ib = inverseBind.find(n);
                bindWorld.push_back(ib != inverseBind.end() ? glm::inverse(ib->second) : M4(n->node_to_world));
            }
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                const ufbx_node* n = nodes[i];
                Joint joint;
                joint.Name = Str(n->name);
                if (joint.Name.empty()) joint.Name = "Joint" + std::to_string(skeleton->Joints.size());
                if (auto it = jointOf.find(n->parent); n->parent && it != jointOf.end()) joint.Parent = it->second;
                glm::mat4 local = joint.Parent >= 0 ? glm::inverse(bindWorld[joint.Parent]) * bindWorld[i] : bindWorld[i];
                glm::vec3 skew;
                glm::vec4 perspective;
                glm::decompose(local, joint.Scale, joint.Rotation, joint.Translation, skew, perspective);
                joint.InverseBind = glm::inverse(bindWorld[i]);
                skeleton->Joints.push_back(joint);
            }
            skeleton->ComputeOrder();
            return skeleton;
        }

        // A mesh instance's triangles into `out`: per material part, vertices welded where every attribute matches.
        void ReadMesh(const ufbx_node* node, const ufbx_mesh* mesh, bool skinnedFile, const std::unordered_map<const ufbx_node*, int>& jointOf,
                      MeshData& out, std::vector<std::pair<const ufbx_mesh*, uint32_t>>& origins)
        {
            const ufbx_skin_deformer* skin = mesh->skin_deformers.count > 0 && mesh->skin_deformers.data[0]->clusters.count > 0
                                           ? mesh->skin_deformers.data[0] : nullptr;
            // Everything goes to world space at bind time: a skinned mesh through its skin's bind pose, the rest through
            // their node; in a skinned file those ride on their nearest bone (a sword in a hand).
            int rigidJoint = -1;
            glm::mat4 toModel(1.0f);
            if (skin)
            {
                const ufbx_skin_cluster* first = skin->clusters.data[0];
                toModel = M4(first->bind_to_world) * M4(first->geometry_to_bone);
            }
            else
            {
                toModel = M4(node->geometry_to_world);
                if (skinnedFile)
                    for (const ufbx_node* p = node; p; p = p->parent)
                        if (auto it = jointOf.find(p); it != jointOf.end())
                        {
                            rigidJoint = it->second;
                            break;
                        }
                if (skinnedFile && rigidJoint < 0) return; // (nothing to move it with)
            }
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(toModel)));
            bool flip = glm::determinant(glm::mat3(toModel)) < 0.0f; // mirrored: winding reverses

            std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
            for (size_t part = 0; part < std::max<size_t>(mesh->material_parts.count, 1); ++part)
            {
                const ufbx_mesh_part* mp = mesh->material_parts.count ? &mesh->material_parts.data[part] : nullptr;
                size_t faceCount = mp ? mp->face_indices.count : mesh->num_faces;
                if (faceCount == 0) continue;
                SubMesh sub;
                sub.IndexOffset = (uint32_t)out.Indices.size();
                const ufbx_material* material = mp && part < node->materials.count ? node->materials.data[part]
                                              : mp && part < mesh->materials.count ? mesh->materials.data[part] : nullptr;
                sub.MaterialSlot = material ? (int32_t)material->typed_id : -1;
                std::unordered_map<CornerKey, uint32_t, CornerHash> welded;
                for (size_t f = 0; f < faceCount; ++f)
                {
                    ufbx_face face = mesh->faces.data[mp ? mp->face_indices.data[f] : f];
                    uint32_t count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                    for (uint32_t t = 0; t < count; ++t)
                        for (int k = 0; k < 3; ++k)
                        {
                            uint32_t corner = tri[t * 3 + (flip ? 2 - k : k)];
                            Vertex v{};
                            v.Position = glm::vec3(toModel * glm::vec4(V3(ufbx_get_vertex_vec3(&mesh->vertex_position, corner)), 1.0f));
                            if (mesh->vertex_normal.exists)
                                v.Normal = glm::normalize(normalMatrix * V3(ufbx_get_vertex_vec3(&mesh->vertex_normal, corner)));
                            if (mesh->vertex_uv.exists)
                            {
                                ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
                                v.UV = { (float)uv.x, 1.0f - (float)uv.y }; // (FBX's V points up)
                            }
                            CornerKey key{ mesh->vertex_indices.data[corner], mesh, v };
                            auto [it, added] = welded.try_emplace(key, (uint32_t)out.Vertices.size());
                            if (added)
                            {
                                out.Vertices.push_back(v);
                                origins.push_back({ mesh, key.FbxVertex });
                                if (skinnedFile)
                                {
                                    SkinVertex sv;
                                    if (skin)
                                    {
                                        // The four strongest weights (ufbx sorts them), made to add up to one.
                                        const ufbx_skin_vertex& sw = skin->vertices.data[key.FbxVertex];
                                        float total = 0.0f;
                                        sv.Weights = glm::vec4(0.0f);
                                        for (uint32_t w = 0; w < std::min<uint32_t>(sw.num_weights, 4); ++w)
                                        {
                                            const ufbx_skin_weight& weight = skin->weights.data[sw.weight_begin + w];
                                            const ufbx_node* bone = skin->clusters.data[weight.cluster_index]->bone_node;
                                            auto joint = jointOf.find(bone);
                                            sv.Joints[w] = joint != jointOf.end() ? (uint32_t)joint->second : 0u;
                                            sv.Weights[w] = (float)weight.weight;
                                            total += (float)weight.weight;
                                        }
                                        if (total > 0.0f) sv.Weights /= total;
                                        else sv.Weights = { 1, 0, 0, 0 };
                                    }
                                    else
                                    {
                                        // Rigid: all on its bone, from the world into that bone's space at bind time.
                                        sv.Joints = glm::uvec4((uint32_t)rigidJoint, 0, 0, 0);
                                        sv.Weights = { 1, 0, 0, 0 };
                                    }
                                    out.Skin.push_back(sv);
                                }
                            }
                            out.Indices.push_back(it->second);
                        }
                }
                sub.IndexCount = (uint32_t)out.Indices.size() - sub.IndexOffset;
                if (sub.IndexCount > 0) out.SubMeshes.push_back(sub);
            }
        }

        // Blend shapes: each blend channel's (last) target shape as offsets on the vertices made from its mesh.
        void ReadBlendShapes(const ufbx_scene* scene, const std::vector<std::pair<const ufbx_mesh*, uint32_t>>& origins, MeshData& out,
                             std::unordered_map<const ufbx_blend_channel*, int>& shapeOf)
        {
            for (size_t m = 0; m < scene->meshes.count; ++m)
            {
                const ufbx_mesh* mesh = scene->meshes.data[m];
                for (size_t d = 0; d < mesh->blend_deformers.count; ++d)
                    for (size_t c = 0; c < mesh->blend_deformers.data[d]->channels.count; ++c)
                    {
                        const ufbx_blend_channel* channel = mesh->blend_deformers.data[d]->channels.data[c];
                        const ufbx_blend_shape* shape = channel->target_shape;
                        if (!shape) continue;
                        // Offsets by FBX vertex.
                        std::unordered_map<uint32_t, std::pair<glm::vec3, glm::vec3>> offsets;
                        for (size_t o = 0; o < shape->num_offsets; ++o)
                            offsets[shape->offset_vertices.data[o]] = { V3(shape->position_offsets.data[o]),
                                                                        shape->normal_offsets.count ? V3(shape->normal_offsets.data[o]) : glm::vec3(0.0f) };
                        BlendShape bs;
                        bs.Name = Str(channel->name);
                        bs.DefaultWeight = std::clamp((float)channel->weight, 0.0f, 1.0f);
                        bs.Positions.assign(out.Vertices.size(), glm::vec3(0.0f));
                        bs.Normals.assign(out.Vertices.size(), glm::vec3(0.0f));
                        bool any = false;
                        for (size_t v = 0; v < origins.size(); ++v)
                            if (origins[v].first == mesh)
                                if (auto it = offsets.find(origins[v].second); it != offsets.end())
                                {
                                    bs.Positions[v] = it->second.first;
                                    bs.Normals[v] = it->second.second;
                                    any = true;
                                }
                        if (!any) continue;
                        shapeOf[channel] = (int)out.BlendShapes.size();
                        out.BlendShapes.push_back(std::move(bs));
                    }
            }
        }

        // Each animation take, baked to keys (linear) for the joints and the blend shape weights.
        void ReadAnimations(const ufbx_scene* scene, const std::unordered_map<const ufbx_node*, int>& jointOf,
                            const std::unordered_map<const ufbx_blend_channel*, int>& shapeOf, MeshData& out)
        {
            for (size_t s = 0; s < scene->anim_stacks.count; ++s)
            {
                const ufbx_anim_stack* stack = scene->anim_stacks.data[s];
                ufbx_bake_opts opts{};
                ufbx_error error;
                ufbx_baked_anim* baked = ufbx_bake_anim(scene, stack->anim, &opts, &error);
                if (!baked)
                {
                    char text[512];
                    ufbx_format_error(text, sizeof(text), &error);
                    Log::Warn("FBX take '{}' could not be read: {}", Str(stack->name), text);
                    continue;
                }
                AnimationClip clip;
                clip.Name = Str(stack->name);
                double begin = baked->playback_time_begin;
                clip.Duration = (float)std::max(baked->playback_duration, 0.0);
                auto time = [&](double t) { return (float)std::max(t - begin, 0.0); };
                for (size_t n = 0; n < baked->nodes.count; ++n)
                {
                    const ufbx_baked_node& bn = baked->nodes.data[n];
                    auto joint = jointOf.find(scene->nodes.data[bn.typed_id]);
                    if (joint == jointOf.end()) continue;
                    auto vec3 = [&](const ufbx_baked_vec3_list& keys, AnimationPath path) {
                        AnimationChannel ch;
                        ch.Joint = joint->second;
                        ch.Path = path;
                        for (size_t k = 0; k < keys.count; ++k)
                        {
                            ch.Times.push_back(time(keys.data[k].time));
                            ch.Values.push_back(glm::vec4(V3(keys.data[k].value), 0.0f));
                        }
                        if (!ch.Times.empty()) clip.Channels.push_back(std::move(ch));
                    };
                    vec3(bn.translation_keys, AnimationPath::Translation);
                    vec3(bn.scale_keys, AnimationPath::Scale);
                    AnimationChannel rot;
                    rot.Joint = joint->second;
                    rot.Path = AnimationPath::Rotation;
                    for (size_t k = 0; k < bn.rotation_keys.count; ++k)
                    {
                        glm::quat q = Q(bn.rotation_keys.data[k].value);
                        rot.Times.push_back(time(bn.rotation_keys.data[k].time));
                        rot.Values.push_back({ q.x, q.y, q.z, q.w });
                    }
                    if (!rot.Times.empty()) clip.Channels.push_back(std::move(rot));
                }
                // Blend shape weights: a channel's DeformPercent (0..100).
                for (size_t e = 0; e < baked->elements.count; ++e)
                {
                    const ufbx_baked_element& be = baked->elements.data[e];
                    const ufbx_element* element = scene->elements.data[be.element_id];
                    if (element->type != UFBX_ELEMENT_BLEND_CHANNEL) continue;
                    auto shape = shapeOf.find(reinterpret_cast<const ufbx_blend_channel*>(element));
                    if (shape == shapeOf.end()) continue;
                    for (size_t p = 0; p < be.props.count; ++p)
                    {
                        if (Str(be.props.data[p].name) != "DeformPercent") continue;
                        AnimationChannel ch;
                        ch.Joint = shape->second;
                        ch.Path = AnimationPath::Translation;
                        for (size_t k = 0; k < be.props.data[p].keys.count; ++k)
                        {
                            ch.Times.push_back(time(be.props.data[p].keys.data[k].time));
                            ch.Values.push_back(glm::vec4((float)be.props.data[p].keys.data[k].value.x / 100.0f, 0, 0, 0));
                        }
                        if (!ch.Times.empty()) clip.WeightChannels.push_back(std::move(ch));
                    }
                }
                ufbx_free_baked_anim(baked);
                if (clip.Duration <= 0.0f)
                    for (const auto& ch : clip.Channels) clip.Duration = std::max(clip.Duration, ch.Times.empty() ? 0.0f : ch.Times.back());
                if (!clip.Channels.empty() || !clip.WeightChannels.empty()) out.Clips.push_back(std::move(clip));
            }
        }
    }

    bool LoadFbx(const fs::path& path, MeshData& out)
    {
        ufbx_load_opts opts{};
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0f;
        opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY; // (the root stays identity: no extra joint)
        opts.generate_missing_normals = true;
        opts.clean_skin_weights = true;
        ufbx_error error;
        std::string file = path.string();
        ufbx_scene* scene = ufbx_load_file(file.c_str(), &opts, &error);
        if (!scene)
        {
            char text[512];
            ufbx_format_error(text, sizeof(text), &error);
            Log::Error("Failed to load FBX '{}': {}", file, text);
            return false;
        }

        std::unordered_map<const ufbx_node*, int> jointOf;
        out.SkeletonData = ReadSkeleton(scene, jointOf);
        bool skinned = out.SkeletonData != nullptr;
        std::vector<std::pair<const ufbx_mesh*, uint32_t>> origins; // per vertex: its mesh and FBX vertex
        size_t skipped = 0;
        for (size_t n = 0; n < scene->nodes.count; ++n)
        {
            const ufbx_node* node = scene->nodes.data[n];
            if (!node->mesh || !node->visible) continue;
            size_t before = out.Vertices.size();
            ReadMesh(node, node->mesh, skinned, jointOf, out, origins);
            if (out.Vertices.size() == before && node->mesh->num_faces > 0) ++skipped;
        }
        std::unordered_map<const ufbx_blend_channel*, int> shapeOf;
        ReadBlendShapes(scene, origins, out, shapeOf);
        if (skinned || !shapeOf.empty()) ReadAnimations(scene, jointOf, shapeOf, out);
        ReadMaterials(scene, path, out);
        ufbx_free_scene(scene);

        if (skipped) Log::Warn("FBX '{}': {} mesh node(s) outside the skeleton ignored in a skinned file", path.filename().string(), skipped);
        if (out.Vertices.empty())
        {
            Log::Error("FBX '{}' contains no meshes", file);
            return false;
        }
        return true;
    }
}
