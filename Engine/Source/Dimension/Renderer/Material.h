#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/Core/Guid.h"
#include "Dimension/Reflection/TypeInfo.h"
#include <glm/glm.hpp>

namespace Dimension
{
    enum class MaterialBlendMode : int32_t { Opaque = 0, Cutout = 1, Transparent = 2 };

    // Metallic/roughness PBR material, stored as a JSON ".mat" asset.
    // Texture conventions follow glTF: metallic-roughness map uses G = roughness, B = metallic.
    struct Material
    {
        glm::vec4 BaseColor{ 1.0f };
        AssetHandle BaseColorMap = AssetHandle::None();
        AssetHandle NormalMap = AssetHandle::None();
        float NormalStrength = 1.0f;
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        AssetHandle MetallicRoughnessMap = AssetHandle::None();
        // Baked ambient occlusion (red channel): darkens the ambient and reflected light, by Occlusion Strength.
        AssetHandle OcclusionMap = AssetHandle::None();
        float OcclusionStrength = 1.0f;
        glm::vec3 EmissionColor{ 0.0f };
        float EmissionStrength = 1.0f;
        AssetHandle EmissionMap = AssetHandle::None();
        glm::vec2 Tiling{ 1.0f };
        glm::vec2 Offset{ 0.0f };
        MaterialBlendMode BlendMode = MaterialBlendMode::Opaque;
        float AlphaCutoff = 0.5f;
        bool DoubleSided = false;
        bool CastShadows = true;
        // Transparent materials (game views): what lies behind is seen bent by the surface's normals (water, glass,
        // ice), this much (0 = straight through, as plain blending).
        float Refraction = 0.0f;

        static const std::vector<FieldInfo>& Fields();

        static Ref<Material> Load(const fs::path& path);
        bool Save(const fs::path& path) const;
    };
}
