#include "Dimension/Particles/VisualEffect.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/Scene.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Core/Log.h"
#include <nlohmann/json.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Dimension
{
    namespace VisualEffects
    {
        namespace
        {
            constexpr glm::vec4 V(float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) { return { x, y, z, w }; }
            const std::vector<const char*> kAttributes = { "Position", "Velocity", "Color", "Size", "Age", "Lifetime", "Rotation", "Angular Velocity" };
            const VfxValue F = VfxValue::Float, Vec = VfxValue::Vector, C = VfxValue::Color;
        }

        const std::vector<NodeType>& NodeTypes()
        {
            static const std::vector<NodeType> types = {
                // Values
                { "Float", "Value/Float", Category::Operator, { { "Value", F, V(1) } }, true, F, {}, "A number" },
                { "Vector", "Value/Vector", Category::Operator, { { "Value", Vec, V(0) } }, true, Vec, {}, "A vector" },
                { "Color", "Value/Color", Category::Operator, { { "Value", C, V(1, 1, 1, 1) } }, true, C, {}, "A color (sRGB, alpha)" },
                { "Property", "Value/Property", Category::Operator, {}, true, F, {}, "An exposed property: set per object (Inspector) or from scripts" },
                // Particle and effect
                { "Get Attribute", "Particle/Get Attribute", Category::Operator, {}, true, Vec, kAttributes, "The particle's attribute now" },
                { "Age Over Lifetime", "Particle/Age Over Lifetime", Category::Operator, {}, true, F, {}, "0 at birth, 1 at death" },
                { "Particle Random", "Particle/Particle Random", Category::Operator, {}, true, Vec, {}, "Four random numbers (0-1) of the particle's own, the same every frame" },
                { "Time", "Effect/Time", Category::Operator, {}, true, F, {}, "Seconds since the effect started" },
                { "Delta Time", "Effect/Delta Time", Category::Operator, {}, true, F, {}, "Seconds this frame" },
                { "Emitter Position", "Effect/Emitter Position", Category::Operator, {}, true, Vec, {}, "The effect's object, in simulation space" },
                { "Source Position", "Event/Source Position", Category::Operator, {}, true, Vec, {}, "Where the event that started the particle was (a child effect's Initialize)" },
                { "Source Velocity", "Event/Source Velocity", Category::Operator, {}, true, Vec, {}, "The velocity of the particle that sent the event" },
                { "Source Color", "Event/Source Color", Category::Operator, {}, true, C, {}, "The color of the particle that sent the event" },
                // Math
                { "Add", "Math/Add", Category::Operator, { { "A", Vec, V(0) }, { "B", Vec, V(0) } }, true, Vec, {}, "A + B" },
                { "Subtract", "Math/Subtract", Category::Operator, { { "A", Vec, V(0) }, { "B", Vec, V(0) } }, true, Vec, {}, "A - B" },
                { "Multiply", "Math/Multiply", Category::Operator, { { "A", Vec, V(1, 1, 1, 1) }, { "B", Vec, V(1, 1, 1, 1) } }, true, Vec, {}, "A * B (per component)" },
                { "Divide", "Math/Divide", Category::Operator, { { "A", Vec, V(1, 1, 1, 1) }, { "B", Vec, V(1, 1, 1, 1) } }, true, Vec, {}, "A / B (per component)" },
                { "Minimum", "Math/Minimum", Category::Operator, { { "A", Vec, V(0) }, { "B", Vec, V(0) } }, true, Vec, {}, "The smaller of A and B" },
                { "Maximum", "Math/Maximum", Category::Operator, { { "A", Vec, V(0) }, { "B", Vec, V(0) } }, true, Vec, {}, "The larger of A and B" },
                { "Power", "Math/Power", Category::Operator, { { "A", Vec, V(1, 1, 1, 1) }, { "B", Vec, V(2, 2, 2, 2) } }, true, Vec, {}, "A to the power B" },
                { "Lerp", "Math/Lerp", Category::Operator, { { "A", Vec, V(0) }, { "B", Vec, V(1, 1, 1, 1) }, { "T", F, V(0.5f) } }, true, Vec, {}, "From A (T = 0) to B (T = 1)" },
                { "Step", "Math/Step", Category::Operator, { { "Edge", Vec, V(0.5f) }, { "X", Vec, V(0) } }, true, Vec, {}, "1 where X >= Edge, else 0" },
                { "Sine", "Math/Sine", Category::Operator, { { "X", Vec, V(0) } }, true, Vec, {}, "sin(X), radians" },
                { "Cosine", "Math/Cosine", Category::Operator, { { "X", Vec, V(0) } }, true, Vec, {}, "cos(X), radians" },
                { "Absolute", "Math/Absolute", Category::Operator, { { "X", Vec, V(0) } }, true, Vec, {}, "|X|" },
                { "Fraction", "Math/Fraction", Category::Operator, { { "X", Vec, V(0) } }, true, Vec, {}, "X minus its floor" },
                { "Floor", "Math/Floor", Category::Operator, { { "X", Vec, V(0) } }, true, Vec, {}, "X rounded down" },
                { "Square Root", "Math/Square Root", Category::Operator, { { "X", Vec, V(1, 1, 1, 1) } }, true, Vec, {}, "sqrt(X)" },
                { "Saturate", "Math/Saturate", Category::Operator, { { "X", Vec, V(0) } }, true, Vec, {}, "X clamped to 0-1" },
                { "Length", "Vector/Length", Category::Operator, { { "X", Vec, V(0) } }, true, F, {}, "The vector's length" },
                { "Normalize", "Vector/Normalize", Category::Operator, { { "X", Vec, V(0, 1, 0) } }, true, Vec, {}, "The vector at length 1" },
                { "Dot", "Vector/Dot", Category::Operator, { { "A", Vec, V(0) }, { "B", Vec, V(0) } }, true, F, {}, "A . B" },
                { "Cross", "Vector/Cross", Category::Operator, { { "A", Vec, V(1, 0, 0) }, { "B", Vec, V(0, 1, 0) } }, true, Vec, {}, "A x B" },
                { "Split", "Vector/Split", Category::Operator, { { "X", Vec, V(0) } }, true, F, { "X", "Y", "Z", "W" }, "One component" },
                { "Combine", "Vector/Combine", Category::Operator, { { "X", F, V(0) }, { "Y", F, V(0) }, { "Z", F, V(0) } }, true, Vec, {}, "A vector from three numbers" },
                // Random
                { "Random Range", "Random/Random Range", Category::Operator, { { "Min", Vec, V(0) }, { "Max", Vec, V(1, 1, 1, 1) } }, true, Vec, {},
                  "A random value between Min and Max (each component on its own)" },
                { "Random In Sphere", "Random/Random In Sphere", Category::Operator, { { "Radius", F, V(1) } }, true, Vec, {}, "A random point inside a sphere" },
                { "Random On Sphere", "Random/Random On Sphere", Category::Operator, { { "Radius", F, V(1) } }, true, Vec, {}, "A random point on a sphere" },
                { "Random In Circle", "Random/Random In Circle", Category::Operator, { { "Radius", F, V(1) } }, true, Vec, {}, "A random point in a flat (XZ) circle" },
                { "Random In Box", "Random/Random In Box", Category::Operator, { { "Size", Vec, V(1, 1, 1) } }, true, Vec, {}, "A random point in a box" },
                { "Random Cone Direction", "Random/Random Cone Direction", Category::Operator, { { "Angle", F, V(25) } }, true, Vec, {},
                  "A random direction within Angle degrees of up (+Y)" },
                // Noise and sampling
                { "Curl Noise", "Noise/Curl Noise", Category::Operator, { { "Position", Vec, V(0) }, { "Frequency", F, V(1) }, { "Strength", F, V(1) } }, true, Vec, {},
                  "A swirling (divergence-free) field at Position" },
                { "Sample Gradient", "Sample/Sample Gradient", Category::Operator, { { "T", F, V(0) } }, true, C, {}, "The node's gradient at T (0-1)" },
                { "Sample Curve", "Sample/Sample Curve", Category::Operator, { { "T", F, V(0) } }, true, F, {}, "The node's curve at T (0-1)" },
                // Blocks
                { "Set Attribute", "Attribute/Set Attribute", Category::Both, { { "Value", Vec, V(0) } }, false, F, kAttributes,
                  "Sets an attribute (Initialize: Position and Velocity in the effect's own space)" },
                { "Add Attribute", "Attribute/Add Attribute", Category::Both, { { "Value", Vec, V(0) } }, false, F, kAttributes, "Adds to an attribute" },
                { "Add Per Second", "Attribute/Add Per Second", Category::Update, { { "Value", Vec, V(0) } }, false, F, kAttributes,
                  "Adds Value times this frame's seconds to an attribute" },
                { "Position (Sphere)", "Shape/Position (Sphere)", Category::Initialize, { { "Center", Vec, V(0) }, { "Radius", F, V(0.5f) }, { "Surface Only", F, V(0) } },
                  false, F, {}, "Starts particles in (or on) a sphere around the effect" },
                { "Position (Box)", "Shape/Position (Box)", Category::Initialize, { { "Center", Vec, V(0) }, { "Size", Vec, V(1, 1, 1) } }, false, F, {},
                  "Starts particles in a box around the effect" },
                { "Position (Circle)", "Shape/Position (Circle)", Category::Initialize, { { "Center", Vec, V(0) }, { "Radius", F, V(1) } }, false, F, {},
                  "Starts particles in a flat circle around the effect" },
                { "Velocity (Cone)", "Velocity/Velocity (Cone)", Category::Initialize, { { "Angle", F, V(25) }, { "Min Speed", F, V(2) }, { "Max Speed", F, V(4) } },
                  false, F, {}, "Sends particles up (+Y of the effect) within a cone" },
                { "Lifetime (Random)", "Attribute/Lifetime (Random)", Category::Initialize, { { "Min", F, V(1) }, { "Max", F, V(2) } }, false, F, {},
                  "A lifetime between Min and Max seconds" },
                { "Gravity", "Forces/Gravity", Category::Update, { { "Force", Vec, V(0, -9.81f, 0) } }, false, F, {}, "Accelerates the particles" },
                { "Drag", "Forces/Drag", Category::Update, { { "Coefficient", F, V(1) } }, false, F, {}, "Slows the particles (per second)" },
                { "Turbulence", "Forces/Turbulence", Category::Update, { { "Strength", F, V(2) }, { "Frequency", F, V(0.5f) } }, false, F, {},
                  "Pushes the particles along a swirling noise field" },
                { "Attractor", "Forces/Attractor", Category::Update, { { "Position", Vec, V(0) }, { "Strength", F, V(5) } }, false, F, {},
                  "Pulls the particles toward a point (the effect's own space)" },
                { "Color Over Life", "Output/Color Over Life", Category::Update, {}, false, F, {}, "The color from the gradient over the particle's life" },
                { "Size Over Life", "Output/Size Over Life", Category::Update, { { "Size", F, V(0.2f) } }, false, F, {},
                  "The size: Size times the curve over the particle's life" },
                { "Collide Plane", "Collision/Collide Plane", Category::Update, { { "Height", F, V(0) }, { "Bounce", F, V(0.5f) }, { "Friction", F, V(0.1f) } },
                  false, F, {}, "Bounces the particles off a level floor" },
                { "Kill Below", "Collision/Kill Below", Category::Update, { { "Height", F, V(-10) } }, false, F, {}, "Removes particles that fall below Height" },
                { "Trigger On Die", "Event/Trigger On Die", Category::Update, { { "Count", F, V(10) } }, false, F, {},
                  "When a particle dies, starts Count particles of the Child effect where it was (they can read its Source attributes)" },
            };
            return types;
        }

        const NodeType* FindType(const std::string& name)
        {
            for (const NodeType& t : NodeTypes())
                if (name == t.Name) return &t;
            return nullptr;
        }

        const char* AttributeName(VfxAttribute a)
        {
            int i = (int)a;
            return i >= 0 && i < (int)kAttributes.size() ? kAttributes[i] : "?";
        }
    }

    // ---- The asset ------------------------------------------------------------------------------------------------

    VfxNode* VisualEffectAsset::Find(uint32_t id)
    {
        for (VfxNode& n : Nodes)
            if (n.Id == id) return &n;
        return nullptr;
    }

    const VfxNode* VisualEffectAsset::Find(uint32_t id) const { return const_cast<VisualEffectAsset*>(this)->Find(id); }

    VfxNode& VisualEffectAsset::Add(const std::string& type, glm::vec2 position, bool toUpdate)
    {
        VfxNode n;
        n.Id = NextId++;
        n.Type = type;
        n.Position = position;
        if (const auto* t = VisualEffects::FindType(type))
        {
            for (const auto& p : t->Inputs) n.Inputs.push_back(p.Default);
            if (type == "Color Over Life") n.Colors = Gradient::Linear({ 1.0f, 0.8f, 0.3f, 1.0f }, { 1.0f, 0.2f, 0.05f, 0.0f });
            if (type == "Size Over Life") n.Values = Curve::Linear(1.0f, 0.0f);
            using VisualEffects::Category;
            if (t->Where == Category::Initialize || (t->Where == Category::Both && !toUpdate)) Initialize.push_back(n.Id);
            else if (t->Where == Category::Update || t->Where == Category::Both) Update.push_back(n.Id);
        }
        Nodes.push_back(n);
        return Nodes.back();
    }

    void VisualEffectAsset::Remove(uint32_t id)
    {
        std::erase_if(Nodes, [&](const VfxNode& n) { return n.Id == id; });
        std::erase_if(Links, [&](const VfxLink& l) { return l.From == id || l.To == id; });
        std::erase(Initialize, id);
        std::erase(Update, id);
    }

    const VfxLink* VisualEffectAsset::LinkInto(uint32_t to, int32_t input) const
    {
        for (const VfxLink& l : Links)
            if (l.To == to && l.Input == input) return &l;
        return nullptr;
    }

    bool VisualEffectAsset::Link(uint32_t from, uint32_t to, int32_t input)
    {
        const VfxNode* source = Find(from);
        const VfxNode* target = Find(to);
        if (!source || !target || from == to) return false;
        const auto* st = VisualEffects::FindType(source->Type);
        const auto* tt = VisualEffects::FindType(target->Type);
        if (!st || !tt || !st->HasOutput || input < 0 || input >= (int32_t)tt->Inputs.size()) return false;
        // No cycles: `to` must not feed `from`.
        std::vector<uint32_t> stack{ from };
        std::set<uint32_t> seen;
        while (!stack.empty())
        {
            uint32_t n = stack.back();
            stack.pop_back();
            if (n == to) return false;
            if (!seen.insert(n).second) continue;
            for (const VfxLink& l : Links)
                if (l.To == n) stack.push_back(l.From);
        }
        std::erase_if(Links, [&](const VfxLink& l) { return l.To == to && l.Input == input; });
        Links.push_back({ from, to, input });
        return true;
    }

    namespace
    {
        nlohmann::json Vec4Json(const glm::vec4& v) { return { v.x, v.y, v.z, v.w }; }
        glm::vec4 JsonVec4(const nlohmann::json& j, glm::vec4 fallback = glm::vec4(0.0f))
        {
            if (!j.is_array()) return fallback;
            for (size_t i = 0; i < 4 && i < j.size(); ++i) fallback[(int)i] = j[i].get<float>();
            return fallback;
        }
        const char* ValueName(VfxValue v) { return v == VfxValue::Color ? "Color" : v == VfxValue::Vector ? "Vector" : "Float"; }
        VfxValue ValueFrom(const std::string& s) { return s == "Color" ? VfxValue::Color : s == "Vector" ? VfxValue::Vector : VfxValue::Float; }
    }

    nlohmann::json VisualEffectAsset::ToJson() const
    {
        nlohmann::json j;
        j["capacity"] = Capacity;
        j["rate"] = Rate;
        j["duration"] = Duration;
        j["loop"] = Loop;
        j["bursts"] = nlohmann::json::array();
        for (const VfxBurst& b : Bursts) j["bursts"].push_back({ { "time", b.Time }, { "count", b.Count } });
        j["space"] = Space == ParticleSpace::World ? "World" : "Local";
        j["blend"] = Blend == ParticleBlend::Additive ? "Additive" : "Alpha";
        j["texture"] = (uint64_t)Texture;
        j["sort"] = Sort;
        j["lit"] = Lit;
        if (Trails) j["trails"] = { { "lifetime", TrailLifetime }, { "width", TrailWidth } };
        j["nextId"] = NextId;
        j["nodes"] = nlohmann::json::array();
        for (const VfxNode& n : Nodes)
        {
            nlohmann::json jn = { { "id", n.Id }, { "type", n.Type }, { "position", { n.Position.x, n.Position.y } }, { "option", n.Option } };
            if (!n.Enabled) jn["enabled"] = false;
            if (n.Child.IsValid()) jn["child"] = (uint64_t)n.Child;
            jn["inputs"] = nlohmann::json::array();
            for (const glm::vec4& v : n.Inputs) jn["inputs"].push_back(Vec4Json(v));
            if (n.Type == "Sample Gradient" || n.Type == "Color Over Life")
            {
                jn["gradient"] = nlohmann::json::array();
                for (const GradientKey& k : n.Colors.Keys) jn["gradient"].push_back({ { "t", k.Time }, { "color", Vec4Json(k.Color) } });
            }
            if (n.Type == "Sample Curve" || n.Type == "Size Over Life")
            {
                jn["curve"] = { { "smooth", n.Values.Smooth }, { "keys", nlohmann::json::array() } };
                for (const CurveKey& k : n.Values.Keys) jn["curve"]["keys"].push_back({ k.Time, k.Value });
            }
            j["nodes"].push_back(jn);
        }
        j["links"] = nlohmann::json::array();
        for (const VfxLink& l : Links) j["links"].push_back({ l.From, l.To, l.Input });
        j["initialize"] = Initialize;
        j["update"] = Update;
        j["properties"] = nlohmann::json::array();
        for (const VfxProperty& p : Properties) j["properties"].push_back({ { "name", p.Name }, { "type", ValueName(p.Type) }, { "value", Vec4Json(p.Value) } });
        return j;
    }

    VisualEffectAsset VisualEffectAsset::FromJson(const nlohmann::json& j)
    {
        VisualEffectAsset a;
        a.Nodes.clear();
        a.Capacity = j.value("capacity", a.Capacity);
        a.Rate = j.value("rate", a.Rate);
        a.Duration = j.value("duration", a.Duration);
        a.Loop = j.value("loop", a.Loop);
        if (j.contains("bursts"))
            for (const auto& b : j["bursts"]) a.Bursts.push_back({ b.value("time", 0.0f), b.value("count", 10) });
        a.Space = j.value("space", std::string("World")) == "Local" ? ParticleSpace::Local : ParticleSpace::World;
        a.Blend = j.value("blend", std::string("Additive")) == "Alpha" ? ParticleBlend::Alpha : ParticleBlend::Additive;
        a.Texture = AssetHandle(j.value("texture", (uint64_t)0));
        a.Sort = j.value("sort", a.Sort);
        a.Lit = j.value("lit", a.Lit);
        if (j.contains("trails"))
        {
            a.Trails = true;
            a.TrailLifetime = j["trails"].value("lifetime", a.TrailLifetime);
            a.TrailWidth = j["trails"].value("width", a.TrailWidth);
        }
        if (j.contains("nodes"))
            for (const auto& jn : j["nodes"])
            {
                VfxNode n;
                n.Id = jn.value("id", 0u);
                n.Type = jn.value("type", std::string());
                if (jn.contains("position")) n.Position = { jn["position"][0].get<float>(), jn["position"][1].get<float>() };
                n.Option = jn.value("option", 0);
                n.Enabled = jn.value("enabled", true);
                n.Child = AssetHandle(jn.value("child", (uint64_t)0));
                if (jn.contains("inputs"))
                    for (const auto& v : jn["inputs"]) n.Inputs.push_back(JsonVec4(v));
                // Inputs a newer version of the type added get their defaults.
                if (const auto* t = VisualEffects::FindType(n.Type))
                    while (n.Inputs.size() < t->Inputs.size()) n.Inputs.push_back(t->Inputs[n.Inputs.size()].Default);
                if (jn.contains("gradient"))
                {
                    n.Colors.Keys.clear();
                    for (const auto& k : jn["gradient"]) n.Colors.Keys.push_back({ k.value("t", 0.0f), JsonVec4(k["color"], glm::vec4(1.0f)) });
                }
                if (jn.contains("curve"))
                {
                    n.Values.Keys.clear();
                    n.Values.Smooth = jn["curve"].value("smooth", false);
                    for (const auto& k : jn["curve"]["keys"]) n.Values.Keys.push_back({ k[0].get<float>(), k[1].get<float>() });
                }
                a.Nodes.push_back(n);
                a.NextId = std::max(a.NextId, n.Id + 1);
            }
        a.NextId = std::max(a.NextId, j.value("nextId", 1u));
        if (j.contains("links"))
            for (const auto& l : j["links"]) a.Links.push_back({ l[0].get<uint32_t>(), l[1].get<uint32_t>(), l[2].get<int32_t>() });
        if (j.contains("initialize")) a.Initialize = j["initialize"].get<std::vector<uint32_t>>();
        if (j.contains("update")) a.Update = j["update"].get<std::vector<uint32_t>>();
        if (j.contains("properties"))
            for (const auto& p : j["properties"]) a.Properties.push_back({ p.value("name", std::string()), ValueFrom(p.value("type", std::string("Float"))), JsonVec4(p["value"]) });
        return a;
    }

    std::shared_ptr<VisualEffectAsset> VisualEffectAsset::Load(const std::filesystem::path& file)
    {
        std::ifstream in(file);
        if (!in) return nullptr;
        try
        {
            nlohmann::json j;
            in >> j;
            return std::make_shared<VisualEffectAsset>(FromJson(j));
        }
        catch (const std::exception& e)
        {
            Log::Error("Visual Effect '{}' could not be read: {}", file.string(), e.what());
            return nullptr;
        }
    }

    bool VisualEffectAsset::Save(const std::filesystem::path& file) const
    {
        std::ofstream out(file);
        if (!out) return false;
        out << ToJson().dump(2);
        return (bool)out;
    }

    VisualEffectAsset VisualEffectAsset::Default()
    {
        VisualEffectAsset a;
        a.Rate = 200.0f;
        a.Capacity = 4000;
        a.Add("Position (Sphere)").Inputs[1] = glm::vec4(0.2f);
        a.Add("Velocity (Cone)");
        a.Add("Lifetime (Random)").Inputs = { glm::vec4(1.0f), glm::vec4(2.0f) };
        auto& size = a.Add("Set Attribute");
        size.Option = (int)VfxAttribute::Size;
        size.Inputs[0] = glm::vec4(0.15f);
        a.Add("Gravity").Inputs[0] = glm::vec4(0.0f, -3.0f, 0.0f, 0.0f);
        a.Add("Turbulence").Inputs = { glm::vec4(1.5f), glm::vec4(0.6f) };
        a.Add("Color Over Life");
        a.Add("Size Over Life").Inputs[0] = glm::vec4(0.15f);
        return a;
    }

    // ---- The compiler ---------------------------------------------------------------------------------------------

    namespace VisualEffects
    {
        namespace
        {
            struct Compiler
            {
                const VisualEffectAsset& Asset;
                VfxProgram& Out;
                std::map<uint32_t, int> Memo; // node -> register (within a block)
                std::map<uint32_t, uint32_t> LutOf;
                std::map<std::tuple<float, float, float, float>, uint32_t> ConstOf;
                int NextReg = 0;

                int Reg()
                {
                    if (NextReg >= VfxProgram::kRegisters)
                    {
                        Out.Errors.push_back("A block's inputs need too many values at once (simplify the graph)");
                        return VfxProgram::kRegisters - 1;
                    }
                    return NextReg++;
                }
                void Emit(VfxOp op, int dst, int a = 0, int b = 0, int c = 0, uint32_t w = 0)
                {
                    Out.Code.push_back({ (uint32_t)op, (uint32_t)dst, (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16), w });
                }
                uint32_t Const(const glm::vec4& v)
                {
                    auto key = std::make_tuple(v.x, v.y, v.z, v.w);
                    if (auto it = ConstOf.find(key); it != ConstOf.end()) return it->second;
                    uint32_t index = (uint32_t)Out.Constants.size();
                    Out.Constants.push_back(v);
                    ConstOf[key] = index;
                    return index;
                }
                int Constant(const glm::vec4& v)
                {
                    int r = Reg();
                    Emit(VfxOp::Const, r, 0, 0, 0, Const(v));
                    return r;
                }
                uint32_t Lut(const VfxNode& n, bool gradient)
                {
                    if (auto it = LutOf.find(n.Id); it != LutOf.end()) return it->second;
                    uint32_t slot = (uint32_t)(Out.Luts.size() / VfxProgram::kLutSize);
                    if (slot >= (uint32_t)VfxProgram::kMaxLuts)
                    {
                        Out.Errors.push_back("Too many gradients and curves (16 at most)");
                        return 0;
                    }
                    for (int i = 0; i < VfxProgram::kLutSize; ++i)
                    {
                        float t = (float)i / (VfxProgram::kLutSize - 1);
                        Out.Luts.push_back(gradient ? n.Colors.Evaluate(t) : glm::vec4(n.Values.Evaluate(t)));
                    }
                    LutOf[n.Id] = slot;
                    return slot;
                }

                // The register holding a node input's value: what is linked into it, else its own value.
                int Input(const VfxNode& n, int input)
                {
                    if (const VfxLink* l = Asset.LinkInto(n.Id, input))
                        if (const VfxNode* from = Asset.Find(l->From)) return Node(*from);
                    glm::vec4 v = input < (int)n.Inputs.size() ? n.Inputs[input] : glm::vec4(0.0f);
                    // A number is the same in every component (so it scales vectors whole).
                    if (const auto* type = FindType(n.Type); type && input < (int)type->Inputs.size() && type->Inputs[input].Kind == VfxValue::Float)
                        v = glm::vec4(v.x);
                    return Constant(v);
                }

                // An operator node's value (computed once per block).
                int Node(const VfxNode& n)
                {
                    if (auto it = Memo.find(n.Id); it != Memo.end()) return it->second;
                    int r = -1;
                    const std::string& t = n.Type;
                    auto unary = [&](VfxOp op) {
                        int a = Input(n, 0);
                        int d = Reg();
                        Emit(op, d, a);
                        return d;
                    };
                    auto binary = [&](VfxOp op) {
                        int a = Input(n, 0), b = Input(n, 1);
                        int d = Reg();
                        Emit(op, d, a, b);
                        return d;
                    };
                    auto input = [&](VfxInput which) {
                        int d = Reg();
                        Emit(VfxOp::Input, d, 0, 0, 0, (uint32_t)which);
                        return d;
                    };
                    if (t == "Float" || t == "Vector" || t == "Color") r = Input(n, 0);
                    else if (t == "Property")
                    {
                        if (n.Option < 0 || n.Option >= (int)Asset.Properties.size())
                        {
                            Out.Errors.push_back("A Property node names no property");
                            r = Constant(glm::vec4(0.0f));
                        }
                        else
                        {
                            r = Reg();
                            Emit(VfxOp::Const, r, 0, 0, 0, (uint32_t)n.Option); // (properties are the first constants)
                        }
                    }
                    else if (t == "Get Attribute")
                    {
                        r = Reg();
                        Emit(VfxOp::GetAttr, r, 0, 0, 0, (uint32_t)std::clamp(n.Option, 0, (int)VfxAttribute::Count - 1));
                    }
                    else if (t == "Age Over Lifetime") r = input(VfxInput::AgeOverLifetime);
                    else if (t == "Particle Random") r = input(VfxInput::ParticleRandom);
                    else if (t == "Time") r = input(VfxInput::Time);
                    else if (t == "Delta Time") r = input(VfxInput::DeltaTime);
                    else if (t == "Emitter Position") r = input(VfxInput::EmitterPosition);
                    else if (t == "Source Position") r = input(VfxInput::SourcePosition);
                    else if (t == "Source Velocity") r = input(VfxInput::SourceVelocity);
                    else if (t == "Source Color") r = input(VfxInput::SourceColor);
                    else if (t == "Add") r = binary(VfxOp::Add);
                    else if (t == "Subtract") r = binary(VfxOp::Sub);
                    else if (t == "Multiply") r = binary(VfxOp::Mul);
                    else if (t == "Divide") r = binary(VfxOp::Div);
                    else if (t == "Minimum") r = binary(VfxOp::Min);
                    else if (t == "Maximum") r = binary(VfxOp::Max);
                    else if (t == "Power") r = binary(VfxOp::Pow);
                    else if (t == "Step") r = binary(VfxOp::Step);
                    else if (t == "Dot") r = binary(VfxOp::Dot);
                    else if (t == "Cross") r = binary(VfxOp::Cross);
                    else if (t == "Lerp")
                    {
                        int a = Input(n, 0), b = Input(n, 1), c = Input(n, 2);
                        r = Reg();
                        Emit(VfxOp::Lerp, r, a, b, c);
                    }
                    else if (t == "Sine") r = unary(VfxOp::Sin);
                    else if (t == "Cosine") r = unary(VfxOp::Cos);
                    else if (t == "Absolute") r = unary(VfxOp::Abs);
                    else if (t == "Fraction") r = unary(VfxOp::Fract);
                    else if (t == "Floor") r = unary(VfxOp::Floor);
                    else if (t == "Square Root") r = unary(VfxOp::Sqrt);
                    else if (t == "Saturate") r = unary(VfxOp::Saturate);
                    else if (t == "Length") r = unary(VfxOp::Length);
                    else if (t == "Normalize") r = unary(VfxOp::Normalize);
                    else if (t == "Split")
                    {
                        int a = Input(n, 0);
                        r = Reg();
                        Emit(VfxOp::Split, r, a, 0, 0, (uint32_t)std::clamp(n.Option, 0, 3));
                    }
                    else if (t == "Combine")
                    {
                        int a = Input(n, 0), b = Input(n, 1), c = Input(n, 2);
                        r = Reg();
                        Emit(VfxOp::Combine, r, a, b, c);
                    }
                    else if (t == "Random Range")
                    {
                        int lo = Input(n, 0), hi = Input(n, 1);
                        int k = Reg();
                        Emit(VfxOp::Random, k);
                        r = Reg();
                        Emit(VfxOp::Lerp, r, lo, hi, k);
                    }
                    else if (t == "Random In Sphere" || t == "Random On Sphere" || t == "Random In Circle")
                    {
                        int radius = Input(n, 0);
                        int k = Reg();
                        Emit(t == "Random In Sphere" ? VfxOp::RandomInSphere : t == "Random On Sphere" ? VfxOp::RandomOnSphere : VfxOp::RandomInCircle, k);
                        r = Reg();
                        Emit(VfxOp::Mul, r, k, radius);
                    }
                    else if (t == "Random In Box")
                    {
                        int size = Input(n, 0);
                        int k = Reg(), half = Constant(glm::vec4(0.5f, 0.5f, 0.5f, 0.0f));
                        Emit(VfxOp::Random, k);
                        int centered = Reg();
                        Emit(VfxOp::Sub, centered, k, half);
                        r = Reg();
                        Emit(VfxOp::Mul, r, centered, size);
                    }
                    else if (t == "Random Cone Direction")
                    {
                        int angle = Input(n, 0);
                        r = Reg();
                        Emit(VfxOp::RandomCone, r, angle);
                    }
                    else if (t == "Curl Noise")
                    {
                        int p = Input(n, 0), f = Input(n, 1), s = Input(n, 2);
                        int scaled = Reg();
                        Emit(VfxOp::Mul, scaled, p, f);
                        int noise = Reg();
                        Emit(VfxOp::Noise, noise, scaled);
                        r = Reg();
                        Emit(VfxOp::Mul, r, noise, s);
                    }
                    else if (t == "Sample Gradient" || t == "Sample Curve")
                    {
                        int tt = Input(n, 0);
                        r = Reg();
                        bool gradient = t == "Sample Gradient";
                        Emit(gradient ? VfxOp::Gradient : VfxOp::Curve, r, tt, 0, 0, Lut(n, gradient));
                    }
                    else
                    {
                        Out.Errors.push_back("'" + t + "' cannot feed a block");
                        r = Constant(glm::vec4(0.0f));
                    }
                    Memo[n.Id] = r;
                    return r;
                }

                // A block's instructions.
                void Block(const VfxNode& n, bool initialize)
                {
                    Memo.clear();
                    NextReg = 0;
                    const std::string& t = n.Type;
                    const auto* type = FindType(t);
                    if (!type || type->HasOutput)
                    {
                        Out.Errors.push_back("'" + t + "' is not a block");
                        return;
                    }
                    if ((type->Where == Category::Initialize && !initialize) || (type->Where == Category::Update && initialize))
                    {
                        Out.Errors.push_back(std::string("'") + t + "' does not belong in " + (initialize ? "Initialize" : "Update"));
                        return;
                    }
                    uint32_t attr = (uint32_t)std::clamp(n.Option, 0, (int)VfxAttribute::Count - 1);
                    auto toSim = [&](int r, bool point) {
                        int d = Reg();
                        Emit(point ? VfxOp::ToSimPoint : VfxOp::ToSimVector, d, r);
                        return d;
                    };
                    auto dtTimes = [&](int r) {
                        int dt = Reg();
                        Emit(VfxOp::Input, dt, 0, 0, 0, (uint32_t)VfxInput::DeltaTime);
                        int d = Reg();
                        Emit(VfxOp::Mul, d, r, dt);
                        return d;
                    };
                    if (t == "Set Attribute" || t == "Add Attribute" || t == "Add Per Second")
                    {
                        int v = Input(n, 0);
                        // Initialize: positions and velocities are given in the effect's own space.
                        if (initialize && (attr == (uint32_t)VfxAttribute::Position || attr == (uint32_t)VfxAttribute::Velocity))
                            v = toSim(v, attr == (uint32_t)VfxAttribute::Position && t == "Set Attribute");
                        if (t == "Add Per Second") v = dtTimes(v);
                        Emit(t == "Set Attribute" ? VfxOp::SetAttr : VfxOp::AddAttr, 0, v, 0, 0, attr);
                    }
                    else if (t == "Position (Sphere)")
                    {
                        int center = Input(n, 0), radius = Input(n, 1), surface = Input(n, 2);
                        int in = Reg(), on = Reg();
                        Emit(VfxOp::RandomInSphere, in);
                        Emit(VfxOp::RandomOnSphere, on);
                        int dir = Reg();
                        Emit(VfxOp::Lerp, dir, in, on, surface);
                        int scaled = Reg();
                        Emit(VfxOp::Mul, scaled, dir, radius);
                        int p = Reg();
                        Emit(VfxOp::Add, p, scaled, center);
                        Emit(VfxOp::SetAttr, 0, toSim(p, true), 0, 0, (uint32_t)VfxAttribute::Position);
                    }
                    else if (t == "Position (Box)")
                    {
                        int center = Input(n, 0), size = Input(n, 1);
                        int k = Reg(), half = Constant(glm::vec4(0.5f, 0.5f, 0.5f, 0.0f));
                        Emit(VfxOp::Random, k);
                        int c = Reg();
                        Emit(VfxOp::Sub, c, k, half);
                        int s = Reg();
                        Emit(VfxOp::Mul, s, c, size);
                        int p = Reg();
                        Emit(VfxOp::Add, p, s, center);
                        Emit(VfxOp::SetAttr, 0, toSim(p, true), 0, 0, (uint32_t)VfxAttribute::Position);
                    }
                    else if (t == "Position (Circle)")
                    {
                        int center = Input(n, 0), radius = Input(n, 1);
                        int k = Reg();
                        Emit(VfxOp::RandomInCircle, k);
                        int s = Reg();
                        Emit(VfxOp::Mul, s, k, radius);
                        int p = Reg();
                        Emit(VfxOp::Add, p, s, center);
                        Emit(VfxOp::SetAttr, 0, toSim(p, true), 0, 0, (uint32_t)VfxAttribute::Position);
                    }
                    else if (t == "Velocity (Cone)")
                    {
                        int angle = Input(n, 0), lo = Input(n, 1), hi = Input(n, 2);
                        int dir = Reg();
                        Emit(VfxOp::RandomCone, dir, angle);
                        int k = Reg();
                        Emit(VfxOp::Random, k);
                        int speed = Reg();
                        Emit(VfxOp::Lerp, speed, lo, hi, k);
                        int v = Reg();
                        Emit(VfxOp::Mul, v, dir, speed);
                        Emit(VfxOp::SetAttr, 0, toSim(v, false), 0, 0, (uint32_t)VfxAttribute::Velocity);
                    }
                    else if (t == "Lifetime (Random)")
                    {
                        int lo = Input(n, 0), hi = Input(n, 1);
                        int k = Reg();
                        Emit(VfxOp::Random, k);
                        int life = Reg();
                        Emit(VfxOp::Lerp, life, lo, hi, k);
                        Emit(VfxOp::SetAttr, 0, life, 0, 0, (uint32_t)VfxAttribute::Lifetime);
                    }
                    else if (t == "Gravity")
                        Emit(VfxOp::AddAttr, 0, dtTimes(Input(n, 0)), 0, 0, (uint32_t)VfxAttribute::Velocity);
                    else if (t == "Drag")
                    {
                        // velocity *= max(1 - coefficient * dt, 0)
                        int k = dtTimes(Input(n, 0));
                        int one = Constant(glm::vec4(1.0f)), zero = Constant(glm::vec4(0.0f));
                        int f = Reg();
                        Emit(VfxOp::Sub, f, one, k);
                        int g = Reg();
                        Emit(VfxOp::Max, g, f, zero);
                        int v = Reg();
                        Emit(VfxOp::GetAttr, v, 0, 0, 0, (uint32_t)VfxAttribute::Velocity);
                        int d = Reg();
                        Emit(VfxOp::Mul, d, v, g);
                        Emit(VfxOp::SetAttr, 0, d, 0, 0, (uint32_t)VfxAttribute::Velocity);
                    }
                    else if (t == "Turbulence")
                    {
                        int strength = Input(n, 0), freq = Input(n, 1);
                        int p = Reg();
                        Emit(VfxOp::GetAttr, p, 0, 0, 0, (uint32_t)VfxAttribute::Position);
                        int time = Reg();
                        Emit(VfxOp::Input, time, 0, 0, 0, (uint32_t)VfxInput::Time);
                        int scroll = Reg(), q = Reg(), s = Reg();
                        Emit(VfxOp::Mul, scroll, time, Constant(glm::vec4(0.0f, 0.0f, 0.3f, 0.0f)));
                        Emit(VfxOp::Mul, q, p, freq);
                        Emit(VfxOp::Add, s, q, scroll);
                        int noise = Reg();
                        Emit(VfxOp::Noise, noise, s);
                        int f = Reg();
                        Emit(VfxOp::Mul, f, noise, strength);
                        Emit(VfxOp::AddAttr, 0, dtTimes(f), 0, 0, (uint32_t)VfxAttribute::Velocity);
                    }
                    else if (t == "Attractor")
                    {
                        int target = toSim(Input(n, 0), true), strength = Input(n, 1);
                        int p = Reg();
                        Emit(VfxOp::GetAttr, p, 0, 0, 0, (uint32_t)VfxAttribute::Position);
                        int d = Reg();
                        Emit(VfxOp::Sub, d, target, p);
                        int dir = Reg();
                        Emit(VfxOp::Normalize, dir, d);
                        int f = Reg();
                        Emit(VfxOp::Mul, f, dir, strength);
                        Emit(VfxOp::AddAttr, 0, dtTimes(f), 0, 0, (uint32_t)VfxAttribute::Velocity);
                    }
                    else if (t == "Color Over Life")
                    {
                        int life = Reg();
                        Emit(VfxOp::Input, life, 0, 0, 0, (uint32_t)VfxInput::AgeOverLifetime);
                        int c = Reg();
                        Emit(VfxOp::Gradient, c, life, 0, 0, Lut(n, true));
                        Emit(VfxOp::SetAttr, 0, c, 0, 0, (uint32_t)VfxAttribute::Color);
                    }
                    else if (t == "Size Over Life")
                    {
                        int size = Input(n, 0);
                        int life = Reg();
                        Emit(VfxOp::Input, life, 0, 0, 0, (uint32_t)VfxInput::AgeOverLifetime);
                        int c = Reg();
                        Emit(VfxOp::Curve, c, life, 0, 0, Lut(n, false));
                        int s = Reg();
                        Emit(VfxOp::Mul, s, c, size);
                        Emit(VfxOp::SetAttr, 0, s, 0, 0, (uint32_t)VfxAttribute::Size);
                    }
                    else if (t == "Collide Plane")
                    {
                        int h = Input(n, 0), b = Input(n, 1), f = Input(n, 2);
                        int params = Reg();
                        Emit(VfxOp::Combine, params, h, b, f);
                        Emit(VfxOp::Bounce, 0, params);
                    }
                    else if (t == "Trigger On Die")
                    {
                        if (!n.Child.IsValid())
                        {
                            Out.Errors.push_back("A Trigger On Die block has no Child effect");
                            return;
                        }
                        auto it = std::find(Out.Children.begin(), Out.Children.end(), n.Child);
                        if (it == Out.Children.end())
                        {
                            if ((int)Out.Children.size() >= VfxProgram::kMaxChildren)
                            {
                                Out.Errors.push_back("Events start at most 4 child effects");
                                return;
                            }
                            it = Out.Children.insert(Out.Children.end(), n.Child);
                        }
                        int count = Input(n, 0);
                        Emit(VfxOp::TriggerDie, 0, count, 0, 0, (uint32_t)(it - Out.Children.begin()));
                    }
                    else if (t == "Kill Below")
                    {
                        int h = Input(n, 0);
                        int p = Reg();
                        Emit(VfxOp::GetAttr, p, 0, 0, 0, (uint32_t)VfxAttribute::Position);
                        int y = Reg();
                        Emit(VfxOp::Split, y, p, 0, 0, 1);
                        int below = Reg();
                        Emit(VfxOp::Step, below, y, h); // h >= y
                        Emit(VfxOp::KillIf, 0, below);
                    }
                }
            };
        }

        glm::vec4 PropertyConstant(const VfxProperty& property, const glm::vec4& value)
        {
            return property.Type == VfxValue::Float ? glm::vec4(value.x) : value;
        }

        VfxProgram Compile(const VisualEffectAsset& asset)
        {
            VfxProgram out;
            Compiler c{ asset, out };
            // Properties are the first constants (their values filled in per object).
            for (const VfxProperty& p : asset.Properties) out.Constants.push_back(PropertyConstant(p, p.Value));
            auto context = [&](const std::vector<uint32_t>& blocks, bool initialize, uint32_t& start, uint32_t& count) {
                start = (uint32_t)out.Code.size();
                for (uint32_t id : blocks)
                    if (const VfxNode* n = asset.Find(id); n && n->Enabled) c.Block(*n, initialize);
                count = (uint32_t)out.Code.size() - start;
            };
            context(asset.Initialize, true, out.InitStart, out.InitCount);
            context(asset.Update, false, out.UpdateStart, out.UpdateCount);
            if (out.Luts.empty()) out.Luts.assign(VfxProgram::kLutSize, glm::vec4(1.0f)); // (the shader always has a table)
            if (out.Constants.empty()) out.Constants.push_back(glm::vec4(0.0f));
            return out;
        }

        // ---- The VM on the CPU (as VfxSim.comp) -------------------------------------------------------------------

        namespace
        {
            uint32_t Hash(uint32_t x)
            {
                x ^= x >> 16;
                x *= 0x7feb352du;
                x ^= x >> 15;
                x *= 0x846ca68bu;
                x ^= x >> 16;
                return x;
            }
            float Unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }
            glm::vec4 Random4(uint32_t index, uint32_t seed, uint32_t pc)
            {
                uint32_t h = Hash(index * 1973u + seed * 9277u + pc * 26699u + 1u);
                uint32_t a = Hash(h), b = Hash(a), c = Hash(b), d = Hash(c);
                return { Unit(a), Unit(b), Unit(c), Unit(d) };
            }
            glm::vec4 Lut(const VfxProgram& p, uint32_t slot, float t)
            {
                size_t base = (size_t)slot * VfxProgram::kLutSize;
                if (base + VfxProgram::kLutSize > p.Luts.size()) return glm::vec4(1.0f);
                float x = std::clamp(t, 0.0f, 1.0f) * (VfxProgram::kLutSize - 1);
                int i = std::min((int)x, VfxProgram::kLutSize - 2);
                return glm::mix(p.Luts[base + i], p.Luts[base + i + 1], x - (float)i);
            }

            glm::vec4 GetAttr(const CpuParticle& p, uint32_t a)
            {
                switch ((VfxAttribute)a)
                {
                case VfxAttribute::Position: return glm::vec4(p.Position, 0.0f);
                case VfxAttribute::Velocity: return glm::vec4(p.Velocity, 0.0f);
                case VfxAttribute::Color: return p.Color;
                case VfxAttribute::Size: return glm::vec4(p.Size);
                case VfxAttribute::Age: return glm::vec4(p.Age);
                case VfxAttribute::Lifetime: return glm::vec4(p.Lifetime);
                case VfxAttribute::Rotation: return glm::vec4(p.Rotation);
                case VfxAttribute::AngularVelocity: return glm::vec4(p.AngularVelocity);
                default: return glm::vec4(0.0f);
                }
            }
            void SetAttr(CpuParticle& p, uint32_t a, const glm::vec4& v)
            {
                switch ((VfxAttribute)a)
                {
                case VfxAttribute::Position: p.Position = glm::vec3(v); break;
                case VfxAttribute::Velocity: p.Velocity = glm::vec3(v); break;
                case VfxAttribute::Color: p.Color = v; break;
                case VfxAttribute::Size: p.Size = v.x; break;
                case VfxAttribute::Age: p.Age = v.x; break;
                case VfxAttribute::Lifetime: p.Lifetime = v.x; break;
                case VfxAttribute::Rotation: p.Rotation = v.x; break;
                case VfxAttribute::AngularVelocity: p.AngularVelocity = v.x; break;
                default: break;
                }
            }

            void Run(const VfxProgram& prog, uint32_t start, uint32_t count, CpuParticle& p, const CpuContext& ctx, uint32_t* onDie = nullptr)
            {
                glm::vec4 r[VfxProgram::kRegisters]{};
                for (uint32_t pc = start; pc < start + count && pc < prog.Code.size(); ++pc)
                {
                    const glm::uvec4& in = prog.Code[pc];
                    uint32_t d = in.y & 31u, a = in.z & 31u, b = (in.z >> 8) & 31u, c = (in.z >> 16) & 31u, w = in.w;
                    switch ((VfxOp)in.x)
                    {
                    case VfxOp::Const: r[d] = w < prog.Constants.size() ? prog.Constants[w] : glm::vec4(0.0f); break;
                    case VfxOp::GetAttr: r[d] = GetAttr(p, w); break;
                    case VfxOp::SetAttr: SetAttr(p, w, r[a]); break;
                    case VfxOp::AddAttr: SetAttr(p, w, GetAttr(p, w) + r[a]); break;
                    case VfxOp::Input:
                        switch ((VfxInput)w)
                        {
                        case VfxInput::AgeOverLifetime: r[d] = glm::vec4(p.Lifetime > 0.0f ? std::clamp(p.Age / p.Lifetime, 0.0f, 1.0f) : 0.0f); break;
                        case VfxInput::Time: r[d] = glm::vec4(ctx.Time); break;
                        case VfxInput::DeltaTime: r[d] = glm::vec4(ctx.DeltaTime); break;
                        case VfxInput::ParticleRandom: r[d] = glm::fract(p.Random * glm::vec4(1.0f, 13.371f, 71.137f, 431.79f) + glm::vec4(0.0f, 0.31f, 0.57f, 0.83f)); break;
                        case VfxInput::ParticleIndex: r[d] = glm::vec4((float)p.Index); break;
                        case VfxInput::EmitterPosition: r[d] = glm::vec4(glm::vec3(ctx.EmitterToSim[3]), 0.0f); break;
                        case VfxInput::SourcePosition: r[d] = ctx.Source ? glm::vec4(ctx.Source->Position, 0.0f) : glm::vec4(0.0f); break;
                        case VfxInput::SourceVelocity: r[d] = ctx.Source ? glm::vec4(ctx.Source->Velocity, 0.0f) : glm::vec4(0.0f); break;
                        case VfxInput::SourceColor: r[d] = ctx.Source ? ctx.Source->Color : glm::vec4(1.0f); break;
                        }
                        break;
                    case VfxOp::Add: r[d] = r[a] + r[b]; break;
                    case VfxOp::Sub: r[d] = r[a] - r[b]; break;
                    case VfxOp::Mul: r[d] = r[a] * r[b]; break;
                    case VfxOp::Div:
                        for (int k = 0; k < 4; ++k) r[d][k] = r[b][k] != 0.0f ? r[a][k] / r[b][k] : 0.0f;
                        break;
                    case VfxOp::Min: r[d] = glm::min(r[a], r[b]); break;
                    case VfxOp::Max: r[d] = glm::max(r[a], r[b]); break;
                    case VfxOp::Lerp: r[d] = glm::mix(r[a], r[b], glm::vec4(r[c].x)); break;
                    case VfxOp::Sin: r[d] = glm::sin(r[a]); break;
                    case VfxOp::Cos: r[d] = glm::cos(r[a]); break;
                    case VfxOp::Abs: r[d] = glm::abs(r[a]); break;
                    case VfxOp::Fract: r[d] = glm::fract(r[a]); break;
                    case VfxOp::Floor: r[d] = glm::floor(r[a]); break;
                    case VfxOp::Sqrt: r[d] = glm::sqrt(glm::max(r[a], glm::vec4(0.0f))); break;
                    case VfxOp::Length: r[d] = glm::vec4(glm::length(glm::vec3(r[a]))); break;
                    case VfxOp::Normalize:
                    {
                        float l = glm::length(glm::vec3(r[a]));
                        r[d] = l > 1e-8f ? glm::vec4(glm::vec3(r[a]) / l, 0.0f) : glm::vec4(0.0f);
                        break;
                    }
                    case VfxOp::Dot: r[d] = glm::vec4(glm::dot(glm::vec3(r[a]), glm::vec3(r[b]))); break;
                    case VfxOp::Cross: r[d] = glm::vec4(glm::cross(glm::vec3(r[a]), glm::vec3(r[b])), 0.0f); break;
                    case VfxOp::Random: r[d] = Random4(p.Index, ctx.Seed, pc); break;
                    case VfxOp::RandomInSphere:
                    case VfxOp::RandomOnSphere:
                    {
                        glm::vec4 k = Random4(p.Index, ctx.Seed, pc);
                        float z = k.x * 2.0f - 1.0f, phi = k.y * glm::two_pi<float>(), s = std::sqrt(std::max(1.0f - z * z, 0.0f));
                        glm::vec3 dir(s * std::cos(phi), z, s * std::sin(phi));
                        r[d] = glm::vec4(dir * ((VfxOp)in.x == VfxOp::RandomInSphere ? std::cbrt(k.z) : 1.0f), 0.0f);
                        break;
                    }
                    case VfxOp::RandomInCircle:
                    {
                        glm::vec4 k = Random4(p.Index, ctx.Seed, pc);
                        float phi = k.x * glm::two_pi<float>(), rr = std::sqrt(k.y);
                        r[d] = glm::vec4(rr * std::cos(phi), 0.0f, rr * std::sin(phi), 0.0f);
                        break;
                    }
                    case VfxOp::RandomCone:
                    {
                        glm::vec4 k = Random4(p.Index, ctx.Seed, pc);
                        float cosMax = std::cos(glm::radians(std::clamp(r[a].x, 0.0f, 180.0f)));
                        float z = glm::mix(1.0f, cosMax, k.x), phi = k.y * glm::two_pi<float>(), s = std::sqrt(std::max(1.0f - z * z, 0.0f));
                        r[d] = glm::vec4(s * std::cos(phi), z, s * std::sin(phi), 0.0f);
                        break;
                    }
                    case VfxOp::Noise: r[d] = glm::vec4(ParticleSystems::CurlNoise(glm::vec3(r[a])), 0.0f); break;
                    case VfxOp::Gradient: r[d] = Lut(prog, w, r[a].x); break;
                    case VfxOp::Curve: r[d] = glm::vec4(Lut(prog, w, r[a].x).x); break;
                    case VfxOp::Split: r[d] = glm::vec4(r[a][(int)std::min(w, 3u)]); break;
                    case VfxOp::Combine: r[d] = glm::vec4(r[a].x, r[b].x, r[c].x, 0.0f); break;
                    case VfxOp::Step: r[d] = glm::step(r[a], r[b]); break;
                    case VfxOp::Saturate: r[d] = glm::clamp(r[a], glm::vec4(0.0f), glm::vec4(1.0f)); break;
                    case VfxOp::Pow: r[d] = glm::pow(glm::max(r[a], glm::vec4(0.0f)), r[b]); break;
                    case VfxOp::KillIf:
                        if (r[a].x > 0.5f) p.Age = std::max(p.Age, p.Lifetime);
                        break;
                    case VfxOp::ToSimPoint: r[d] = glm::vec4(glm::vec3(ctx.EmitterToSim * glm::vec4(glm::vec3(r[a]), 1.0f)), 0.0f); break;
                    case VfxOp::ToSimVector: r[d] = glm::vec4(glm::vec3(ctx.EmitterToSim * glm::vec4(glm::vec3(r[a]), 0.0f)), 0.0f); break;
                    case VfxOp::TriggerDie:
                        if (onDie && w < (uint32_t)VfxProgram::kMaxChildren) onDie[w] = (uint32_t)std::max(std::lround(r[a].x), 0L);
                        break;
                    case VfxOp::Bounce:
                    {
                        // r[a] = (height, bounce, friction): below the plane and moving down, it comes back up.
                        float h = r[a].x;
                        if (p.Position.y < h)
                        {
                            p.Position.y = h;
                            if (p.Velocity.y < 0.0f)
                            {
                                p.Velocity.y = -p.Velocity.y * r[a].y;
                                p.Velocity.x *= 1.0f - std::clamp(r[a].z, 0.0f, 1.0f);
                                p.Velocity.z *= 1.0f - std::clamp(r[a].z, 0.0f, 1.0f);
                            }
                        }
                        break;
                    }
                    default: break;
                    }
                }
            }
        }

        void Spawn(const VfxProgram& program, CpuParticle& p, const CpuContext& ctx)
        {
            uint32_t h = Hash(p.Index * 7919u + ctx.Seed * 104729u + 17u);
            p.Position = ctx.Source ? ctx.Source->Position : glm::vec3(ctx.EmitterToSim[3]);
            p.Velocity = glm::vec3(0.0f);
            p.Color = glm::vec4(1.0f);
            p.Size = 0.1f;
            p.Age = 0.0f;
            p.Lifetime = 1.0f;
            p.Rotation = p.AngularVelocity = 0.0f;
            p.Random = Unit(h);
            Run(program, program.InitStart, program.InitCount, p, ctx);
            p.Lifetime = std::max(p.Lifetime, 1e-4f); // (as VfxSim.comp: none dies at once)
        }

        void Step(const VfxProgram& program, CpuParticle& p, const CpuContext& ctx)
        {
            if (!p.Alive()) return;
            uint32_t onDie[VfxProgram::kMaxChildren] = {};
            Run(program, program.UpdateStart, program.UpdateCount, p, ctx, onDie);
            p.Position += p.Velocity * ctx.DeltaTime;
            p.Rotation += p.AngularVelocity * ctx.DeltaTime;
            p.Age += ctx.DeltaTime;
            p.Lifetime = std::max(p.Lifetime, 1e-4f);
            // Died this step: its events (as VfxSim.comp writes them).
            if (!p.Alive() && ctx.Events)
                for (int s = 0; s < VfxProgram::kMaxChildren; ++s)
                    for (uint32_t k = 0; k < onDie[s]; ++k) ctx.Events->push_back({ s, p.Position, p.Velocity, p.Color });
        }

        // ---- Programs and spawning --------------------------------------------------------------------------------

        namespace
        {
            struct ProgramCache
            {
                std::mutex Mutex;
                std::unordered_map<AssetHandle, std::shared_ptr<const VfxProgram>> Programs;
            };
            ProgramCache& Cache()
            {
                static ProgramCache cache;
                return cache;
            }
        }

        std::shared_ptr<const VfxProgram> ProgramOf(AssetHandle asset)
        {
            if (!asset.IsValid()) return nullptr;
            auto& cache = Cache();
            {
                std::lock_guard lock(cache.Mutex);
                if (auto it = cache.Programs.find(asset); it != cache.Programs.end()) return it->second;
            }
            auto graph = AssetManager::GetVisualEffect(asset);
            if (!graph) return nullptr;
            auto program = std::make_shared<VfxProgram>(Compile(*graph));
            for (const std::string& e : program->Errors) Log::Warn("Visual Effect '{}': {}", AssetManager::GetMetadata(asset) ? AssetManager::GetMetadata(asset)->Path.string() : "?", e);
            std::lock_guard lock(cache.Mutex);
            cache.Programs[asset] = program;
            return program;
        }

        void Invalidate(AssetHandle asset)
        {
            auto& cache = Cache();
            std::lock_guard lock(cache.Mutex);
            if (asset.IsValid()) cache.Programs.erase(asset);
            else cache.Programs.clear();
        }

        void Update(Scene& scene, float dt)
        {
            for (auto [e, vfx, t] : scene.GetRegistry().ViewOf<VisualEffectComponent, TransformComponent>())
            {
                if (!t.ActiveInHierarchy) continue;
                auto graph = AssetManager::GetVisualEffect(vfx.Asset);
                if (!graph) continue;
                // Edit mode previews effects that play on awake (the Scene view, as Unity's).
                if (!scene.IsRunning() && vfx.PlayOnAwake && !vfx.Playing && vfx.Time == 0.0f) vfx.Play();
                float step = std::max(dt * vfx.PlayRate, 0.0f);
                vfx.PendingDt += step;
                vfx.SinceSpawn += step;
                if (!vfx.Playing || step <= 0.0f) continue;
                float before = vfx.Time, after = vfx.Time + step;
                vfx.Time = after;
                float duration = std::max(graph->Duration, 0.01f);
                if (!graph->Loop) after = std::min(after, duration);
                int count = 0;
                // The rate, carrying fractions of a particle to later frames.
                float owed = vfx.SpawnCarry + std::max(graph->Rate, 0.0f) * std::max(after - before, 0.0f);
                count += (int)owed;
                vfx.SpawnCarry = owed - std::floor(owed);
                // Bursts whose time came this frame (every loop).
                for (const VfxBurst& b : graph->Bursts)
                {
                    float first = std::floor(before / duration), last = std::floor(after / duration);
                    for (float loop = first; loop <= last; loop += 1.0f)
                    {
                        float at = loop * duration + std::max(b.Time, 0.0f);
                        if (at >= before && at < after && (graph->Loop || loop == 0.0f)) count += std::max(b.Count, 0);
                    }
                }
                if (count > 0)
                {
                    vfx.PendingSpawn += count;
                    vfx.SinceSpawn = 0.0f;
                }
                if (!graph->Loop && vfx.Time >= duration) vfx.Playing = false;
            }
        }

        void Restart(Scene& scene)
        {
            for (auto [e, vfx] : scene.GetRegistry().ViewOf<VisualEffectComponent>())
            {
                vfx.Reinit();
                if (vfx.PlayOnAwake) vfx.Play();
                else vfx.Playing = false;
            }
        }
    }

    // ---- The component --------------------------------------------------------------------------------------------

    void VisualEffectComponent::Play()
    {
        if (!Playing) Time = 0.0f;
        Playing = true;
    }

    void VisualEffectComponent::Reinit()
    {
        Playing = false;
        Time = 0.0f;
        SpawnCarry = 0.0f;
        PendingSpawn = 0;
        PendingDt = 0.0f;
        SinceSpawn = 1e9f;
        AliveCount = 0;
        ++Generation;
    }

    bool VisualEffectComponent::SetProperty(const std::string& name, const glm::vec4& value)
    {
        auto graph = AssetManager::GetVisualEffect(Asset);
        if (!graph || std::none_of(graph->Properties.begin(), graph->Properties.end(), [&](const VfxProperty& p) { return p.Name == name; })) return false;
        for (VfxOverride& o : Overrides)
            if (o.Name == name)
            {
                o.Value = value;
                return true;
            }
        Overrides.push_back({ name, value });
        return true;
    }

    bool VisualEffectComponent::GetProperty(const std::string& name, glm::vec4& value) const
    {
        for (const VfxOverride& o : Overrides)
            if (o.Name == name)
            {
                value = o.Value;
                return true;
            }
        auto graph = AssetManager::GetVisualEffect(Asset);
        if (!graph) return false;
        for (const VfxProperty& p : graph->Properties)
            if (p.Name == name)
            {
                value = p.Value;
                return true;
            }
        return false;
    }
}
