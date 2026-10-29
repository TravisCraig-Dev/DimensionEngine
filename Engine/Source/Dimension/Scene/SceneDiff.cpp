#include "Dimension/Scene/SceneDiff.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace Dimension::SceneDiff
{
    namespace
    {
        using json = nlohmann::json;

        struct Records
        {
            std::map<uint64_t, const json*> ByGuid;
            std::vector<uint64_t> Order;
        };

        Records Index(const json& scene)
        {
            Records r;
            if (!scene.contains("entities") || !scene["entities"].is_array()) return r;
            for (const json& e : scene["entities"])
            {
                uint64_t guid = e.value("guid", uint64_t(0));
                r.ByGuid[guid] = &e;
                r.Order.push_back(guid);
            }
            return r;
        }

        std::string NameOf(const json& record)
        {
            if (record.contains("components") && record["components"].contains("Tag"))
                return record["components"]["Tag"].value("Name", std::string("Object"));
            return "Object";
        }

        // "A", "A and B", "3 objects".
        std::string Names(const std::vector<const json*>& records)
        {
            if (records.size() == 1) return NameOf(*records[0]);
            if (records.size() == 2) return NameOf(*records[0]) + " and " + NameOf(*records[1]);
            return std::to_string(records.size()) + " objects";
        }

        // The records that are not under another one of them (a prefab instance counts once).
        std::vector<const json*> Roots(const std::vector<const json*>& records)
        {
            std::set<uint64_t> guids;
            for (const json* r : records) guids.insert(r->value("guid", uint64_t(0)));
            std::vector<const json*> out;
            for (const json* r : records)
                if (!guids.contains(r->value("parent", uint64_t(0)))) out.push_back(r);
            return out;
        }

        // The fields of a component that differ (its own keys, a few at most).
        std::vector<std::string> ChangedFields(const json& a, const json& b)
        {
            std::vector<std::string> out;
            if (!a.is_object() || !b.is_object()) return out;
            std::set<std::string> keys;
            for (auto& [k, v] : a.items()) keys.insert(k);
            for (auto& [k, v] : b.items()) keys.insert(k);
            for (const std::string& k : keys)
                if (!a.contains(k) || !b.contains(k) || a[k] != b[k]) out.push_back(k);
            return out;
        }
    }

    std::string Describe(const json& before, const json& after)
    {
        Records a = Index(before), b = Index(after);
        std::vector<const json*> added, removed;
        for (auto& [guid, record] : b.ByGuid)
            if (!a.ByGuid.contains(guid)) added.push_back(record);
        for (auto& [guid, record] : a.ByGuid)
            if (!b.ByGuid.contains(guid)) removed.push_back(record);
        // In scene order.
        auto byOrder = [](const Records& r, std::vector<const json*>& list) {
            std::sort(list.begin(), list.end(), [&](const json* x, const json* y) {
                auto at = [&](const json* j) {
                    return std::find(r.Order.begin(), r.Order.end(), j->value("guid", uint64_t(0))) - r.Order.begin();
                };
                return at(x) < at(y);
            });
        };
        byOrder(b, added);
        byOrder(a, removed);
        std::vector<std::string> parts;
        if (!added.empty()) parts.push_back("Create " + Names(Roots(added)));
        if (!removed.empty()) parts.push_back("Delete " + Names(Roots(removed)));

        // Objects in both that differ.
        std::vector<std::pair<const json*, const json*>> changed;
        for (uint64_t guid : b.Order)
            if (auto it = a.ByGuid.find(guid); it != a.ByGuid.end() && *it->second != *b.ByGuid[guid]) changed.push_back({ it->second, b.ByGuid[guid] });
        if (changed.size() == 1)
        {
            const json& x = *changed[0].first;
            const json& y = *changed[0].second;
            std::string name = NameOf(y);
            const json& cx = x.value("components", json::object());
            const json& cy = y.value("components", json::object());
            std::vector<std::string> addedComponents, removedComponents, editedComponents;
            for (auto& [k, v] : cy.items())
                if (!cx.contains(k)) addedComponents.push_back(k);
                else if (cx[k] != v) editedComponents.push_back(k);
            for (auto& [k, v] : cx.items())
                if (!cy.contains(k)) removedComponents.push_back(k);
            if (x.value("parent", uint64_t(0)) != y.value("parent", uint64_t(0))) parts.push_back("Reparent " + name);
            for (const auto& c : addedComponents) parts.push_back("Add " + c + " to " + name);
            for (const auto& c : removedComponents) parts.push_back("Remove " + c + " from " + name);
            if (editedComponents.size() == 1 && editedComponents[0] == "Tag" &&
                ChangedFields(cx["Tag"], cy["Tag"]) == std::vector<std::string>{ "Name" })
                parts.push_back("Rename " + NameOf(x) + " to " + name);
            else if (editedComponents.size() == 1)
            {
                auto fields = ChangedFields(cx[editedComponents[0]], cy[editedComponents[0]]);
                std::string list;
                for (size_t i = 0; i < fields.size() && i < 3; ++i) list += (i ? ", " : "") + fields[i];
                if (fields.size() > 3) list += ", ...";
                parts.push_back(name + ": " + editedComponents[0] + (list.empty() ? "" : " (" + list + ")"));
            }
            else if (!editedComponents.empty())
            {
                std::string list;
                for (size_t i = 0; i < editedComponents.size(); ++i) list += (i ? ", " : "") + editedComponents[i];
                parts.push_back(name + ": " + list);
            }
        }
        else if (changed.size() > 1)
        {
            // The same single component on each: say which.
            std::set<std::string> components;
            bool single = true;
            for (auto& [x, y] : changed)
            {
                std::vector<std::string> edited;
                const json& cx = x->value("components", json::object());
                const json& cy = y->value("components", json::object());
                for (auto& [k, v] : cy.items())
                    if (!cx.contains(k) || cx[k] != v) edited.push_back(k);
                for (auto& [k, v] : cx.items())
                    if (!cy.contains(k)) edited.push_back(k);
                if (edited.size() != 1) single = false;
                components.insert(edited.begin(), edited.end());
            }
            if (single && components.size() == 1) parts.push_back(*components.begin() + " on " + std::to_string(changed.size()) + " objects");
            else parts.push_back("Edit " + std::to_string(changed.size()) + " objects");
        }

        if (before.value("settings", json::object()) != after.value("settings", json::object())) parts.push_back("Scene Settings");
        if (before.value("name", std::string()) != after.value("name", std::string())) parts.push_back("Rename Scene");
        if (parts.empty())
        {
            // Same objects, same values: only their order (the Hierarchy) moved.
            std::vector<uint64_t> kept;
            for (uint64_t g : a.Order)
                if (b.ByGuid.contains(g)) kept.push_back(g);
            return kept != b.Order ? "Reorder objects" : "No change";
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) out += (i ? "; " : "") + parts[i];
        return out;
    }
}
