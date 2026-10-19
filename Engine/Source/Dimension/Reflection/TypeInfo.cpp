#include "Dimension/Reflection/TypeInfo.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>

namespace Dimension
{
    using json = nlohmann::json;

    namespace
    {
        template <int N> json VecToJson(const float* v) { return json(std::vector<float>(v, v + N)); }
        template <int N> void VecFromJson(float* v, const json& j)
        {
            if (!j.is_array()) return;
            for (int i = 0; i < N && i < (int)j.size(); ++i) v[i] = j[i].get<float>();
        }
    }

    json FieldToJson(const FieldInfo& field, const void* object, const EntityRefResolver* resolver)
    {
        const void* p = field.Access(const_cast<void*>(object));
        switch (field.Type)
        {
        case FieldType::Bool: return *(const bool*)p;
        case FieldType::Int:
        case FieldType::Enum: return *(const int32_t*)p;
        case FieldType::Float: return *(const float*)p;
        case FieldType::Vec2: return VecToJson<2>((const float*)p);
        case FieldType::Vec3:
        case FieldType::Color3:
        case FieldType::Angles: return VecToJson<3>((const float*)p);
        case FieldType::Vec4:
        case FieldType::Color4: return VecToJson<4>((const float*)p);
        case FieldType::String: return *(const std::string*)p;
        case FieldType::Texture:
        case FieldType::Mesh:
        case FieldType::Scene:
        case FieldType::Prefab:
        case FieldType::Audio:
        case FieldType::Material:
        case FieldType::Font:
        case FieldType::AnimatorController: return (uint64_t) * (const Guid*)p;
        case FieldType::EntityRef:
        {
            Entity e = *(const Entity*)p;
            return resolver && !e.IsNull() ? resolver->ToGuid(e) : uint64_t(0);
        }
        case FieldType::Curve:
        {
            json keys = json::array();
            for (auto& k : ((const Curve*)p)->Keys) keys.push_back({ k.Time, k.Value });
            return keys;
        }
        case FieldType::Gradient:
        {
            json keys = json::array();
            for (auto& k : ((const Gradient*)p)->Keys) keys.push_back({ k.Time, k.Color.r, k.Color.g, k.Color.b, k.Color.a });
            return keys;
        }
        case FieldType::Vec3List:
        {
            json list = json::array();
            for (auto& v : *(const std::vector<glm::vec3>*)p) list.push_back({ v.x, v.y, v.z });
            return list;
        }
        case FieldType::StringList: return *(const std::vector<std::string>*)p;
        }
        return nullptr;
    }

    void FieldFromJson(const FieldInfo& field, void* object, const json& v, const EntityRefResolver* resolver)
    {
        if (v.is_null()) return;
        void* p = field.Access(object);
        try
        {
            switch (field.Type)
            {
            case FieldType::Bool: *(bool*)p = v.get<bool>(); break;
            case FieldType::Int:
            case FieldType::Enum: *(int32_t*)p = v.get<int32_t>(); break;
            case FieldType::Float: *(float*)p = v.get<float>(); break;
            case FieldType::Vec2: VecFromJson<2>((float*)p, v); break;
            case FieldType::Vec3:
            case FieldType::Color3:
            case FieldType::Angles: VecFromJson<3>((float*)p, v); break;
            case FieldType::Vec4:
            case FieldType::Color4: VecFromJson<4>((float*)p, v); break;
            case FieldType::String: *(std::string*)p = v.get<std::string>(); break;
            case FieldType::Texture:
            case FieldType::Mesh:
            case FieldType::Scene:
            case FieldType::Prefab:
            case FieldType::Audio:
            case FieldType::Material:
            case FieldType::Font:
            case FieldType::AnimatorController: *(Guid*)p = Guid(v.get<uint64_t>()); break;
            case FieldType::EntityRef:
                *(Entity*)p = resolver ? resolver->FromGuid(v.get<uint64_t>()) : Entity::Null();
                break;
            case FieldType::Curve:
            {
                if (!v.is_array()) break;
                Curve c;
                for (auto& k : v)
                    if (k.is_array() && k.size() >= 2) c.Keys.push_back({ k[0].get<float>(), k[1].get<float>() });
                c.Sort();
                if (!c.Keys.empty()) *(Curve*)p = std::move(c);
                break;
            }
            case FieldType::Gradient:
            {
                if (!v.is_array()) break;
                Gradient g;
                for (auto& k : v)
                    if (k.is_array() && k.size() >= 5)
                        g.Keys.push_back({ k[0].get<float>(), { k[1].get<float>(), k[2].get<float>(), k[3].get<float>(), k[4].get<float>() } });
                g.Sort();
                if (!g.Keys.empty()) *(Gradient*)p = std::move(g);
                break;
            }
            case FieldType::Vec3List:
            {
                if (!v.is_array()) break;
                auto& list = *(std::vector<glm::vec3>*)p;
                list.clear();
                for (auto& k : v)
                    if (k.is_array() && k.size() >= 3) list.push_back({ k[0].get<float>(), k[1].get<float>(), k[2].get<float>() });
                break;
            }
            case FieldType::StringList:
            {
                if (!v.is_array()) break;
                auto& list = *(std::vector<std::string>*)p;
                list.clear();
                for (auto& k : v)
                    if (k.is_string()) list.push_back(k.get<std::string>());
                break;
            }
            }
        }
        catch (const json::exception& e)
        {
            Log::Warn("Failed to read field '{}': {}", field.Name, e.what());
        }
    }
}
