#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace BodyTracking
{
inline uint32_t ItemForm(std::string_view name)
{
    // Existing Tilted equipped-model root naming contract; no weapon-ID table.
    if (name.size() < 12 || name.back() != ')' || name[name.size()-10] != '(' || name[name.size()-11] != ' ') return 0;
    uint32_t id{};
    for (size_t i = name.size() - 9; i < name.size() - 1; ++i)
    {
        const char c = name[i];
        const int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
        if (nibble < 0) return 0;
        id = (id << 4) | nibble;
    }
    return id;
}
struct AttachedItem
{
    int Index{-1};
    uint32_t Form{};
    bool Ambiguous{}, InvalidTree{};
};

// Reads only the copied tree. Hand order matches BodyPose: right=0, left=1.
// Sender requests any unique attached item; receiver additionally matches the
// exact mapped form. Both use the same ancestry and ambiguity contract.
template<class Nodes>
AttachedItem SelectAttachedItem(const Nodes& nodes, size_t hand, uint32_t requestedForm = 0)
{
    AttachedItem result;
    if (hand > 1) { result.InvalidTree = true; return result; }
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const uint32_t form = ItemForm(nodes[i].Name);
        if (!form || (requestedForm && form != requestedForm)) continue;
        int parent = nodes[i].Parent;
        bool attached{};
        size_t depth{};
        for (; parent >= 0 && depth < 128; ++depth)
        {
            if (static_cast<size_t>(parent) >= nodes.size()) { result.InvalidTree = true; return result; }
            if (nodes[parent].Name == (hand == 0 ? "WEAPON" : "SHIELD")) { attached = true; break; }
            parent = nodes[parent].Parent;
        }
        if (!attached && parent >= 0) { result.InvalidTree = true; return result; }
        if (!attached) continue;
        if (result.Index >= 0) { result.Ambiguous = true; return result; }
        result.Index = static_cast<int>(i);
        result.Form = form;
    }
    return result;
}
}
