#include "Dimension/Renderer/Material.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace Dimension
{
    const std::vector<FieldInfo>& Material::Fields()
    {
        static std::vector<FieldInfo> fields = [] {
            std::vector<FieldInfo> f;
            FieldListBuilder<Material>(f)
                .Field("Base Color", &Material::BaseColor).As(FieldType::Color4)
                .Field("Base Map", &Material::BaseColorMap).As(FieldType::Texture)
                .Field("Normal Map", &Material::NormalMap).As(FieldType::Texture)
                .Field("Normal Strength", &Material::NormalStrength).Range(0.0f, 4.0f).Speed(0.01f)
                .Field("Metallic", &Material::Metallic).Range(0.0f, 1.0f).Speed(0.01f)
                .Field("Roughness", &Material::Roughness).Range(0.02f, 1.0f).Speed(0.01f)
                .Field("Metallic Roughness Map", &Material::MetallicRoughnessMap).As(FieldType::Texture)
                .Field("Occlusion Map", &Material::OcclusionMap).As(FieldType::Texture)
                    .Tooltip("Baked ambient occlusion (red channel): darkens ambient light and reflections in creases")
                .Field("Occlusion Strength", &Material::OcclusionStrength).Range(0.0f, 1.0f).Speed(0.01f)
                    .Tooltip("glTF convention: G = roughness, B = metallic (multiplied with the values above)")
                .Field("Emission Color", &Material::EmissionColor).As(FieldType::Color3)
                .Field("Emission Strength", &Material::EmissionStrength).Range(0.0f, 100.0f).Speed(0.05f)
                .Field("Emission Map", &Material::EmissionMap).As(FieldType::Texture)
                .Field("Tiling", &Material::Tiling)
                .Field("Offset", &Material::Offset)
                .Field("Blend Mode", &Material::BlendMode).Enum({ "Opaque", "Cutout", "Transparent" })
                .Field("Alpha Cutoff", &Material::AlphaCutoff).Range(0.0f, 1.0f).Speed(0.01f)
                .Field("Double Sided", &Material::DoubleSided)
                .Field("Cast Shadows", &Material::CastShadows)
                .Field("Refraction", &Material::Refraction).Range(0.0f, 2.0f).Speed(0.01f)
                    .Tooltip("Transparent: bends what is seen through it by the surface's normals (water, glass; game views)")
                .Field("Translucency", &Material::Translucency).Range(0.0f, 1.0f).Speed(0.01f)
                    .Tooltip("Thin surfaces (leaves, needles, grass, cloth): sunlight shining through from behind");
            return f;
        }();
        return fields;
    }

    Ref<Material> Material::Load(const fs::path& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            Log::Error("Cannot open material '{}'", path.string());
            return nullptr;
        }
        auto material = MakeRef<Material>();
        try
        {
            auto j = nlohmann::json::parse(in);
            for (auto& f : Fields())
                if (j.contains(f.Name)) FieldFromJson(f, material.get(), j[f.Name], nullptr);
        }
        catch (const std::exception& e)
        {
            Log::Error("Invalid material '{}': {}", path.string(), e.what());
        }
        return material;
    }

    bool Material::Save(const fs::path& path) const
    {
        nlohmann::json j;
        for (auto& f : Fields()) j[f.Name] = FieldToJson(f, this, nullptr);
        std::ofstream out(path);
        out << j.dump(2);
        return (bool)out;
    }
}
