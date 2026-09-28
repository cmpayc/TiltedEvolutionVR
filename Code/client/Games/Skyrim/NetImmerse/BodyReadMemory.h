#pragma once

#include <Structs/BodyPoseSource.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BodyTracking
{
// A span the backend proved readable. It carries no permission flags: writability is proven per
// page on demand and executability per address, so two probes with different protections can
// merge into one readable interval.
struct BodyMemoryInterval { uintptr_t Begin{}, End{}; };
// A VirtualQuery region as the vq backend and the test fixture describe memory.
struct BodyMemoryRegion { uintptr_t Begin{}, End{}; bool Write{}, Execute{}; };

// One native invocation only. Backend contract (all stats-bearing calls take the invocation's stats):
//   bool Probe(uintptr_t p, size_t bytes, BodyMemoryInterval& out, NativeReadStats&)
//        proves [p, p+bytes) readable at the instant of the call; out contains p and may be a superset
//        (rpm: exactly the page-rounded span, all of it copied; vq: exactly one VirtualQuery region)
//   bool Writable(uintptr_t begin, uintptr_t end, NativeReadStats&)  a span Probe already accepted (backend owns granularity)
//   bool Executable(uintptr_t p, NativeReadStats&)       p is inside executable code
//   bool KnownReadOnly(uintptr_t p, size_t bytes)        the whole span is known non-writable (rpm: never)
//   bool MergeIntervals() const                          rpm merges page spans; vq keeps regions as-is
//   uintptr_t Rtti(uintptr_t p, uintptr_t function)      the verified GetRTTI dispatch
// Tests use real local buffers with explicit regions through the same contract.
template<class Backend> class BodyReadMemory
{
public:
    static constexpr size_t kIntervalCap = 4096;

    Backend Access;
    NativeReadStats Stats;
    std::vector<BodyMemoryInterval> Intervals; // sorted by Begin, disjoint

    bool Range(uintptr_t p, size_t bytes, bool write = false)
    {
        ++Stats.RangeCalls;
        if (!p || bytes > UINTPTR_MAX - p) return false;
        if (!Cover(p, p + bytes)) return false;
        return !write || Access.Writable(p, p + bytes, Stats);
    }
    template<class T> bool Read(uintptr_t p, T& value)
    {
        if (!Range(p, sizeof(T))) return false;
        std::memcpy(&value, reinterpret_cast<const void*>(p), sizeof(T));
        return true;
    }
    bool Name(uintptr_t p, std::string& name, bool* immutable = nullptr)
    {
        name.clear();
        if (!p) return true; // unnamed geometry is legitimate
        std::array<char, 128> text{};
        size_t copied{};
        while (copied < text.size())
        {
            if (!Cover(p, p + 1)) return false;
            const auto* interval = Lookup(p);
            if (!interval) return false;
            const auto count = std::min(text.size() - copied, static_cast<size_t>(interval->End - p));
            if (immutable && !Access.KnownReadOnly(p, count)) *immutable = false;
            ++Stats.NameChunks;
            std::memcpy(text.data() + copied, reinterpret_cast<const void*>(p), count);
            if (const auto* zero = static_cast<const char*>(std::memchr(text.data() + copied, 0, count)))
            {
                name.assign(text.data(), static_cast<size_t>(zero - text.data()));
                return true;
            }
            copied += count;
            p += count;
        }
        return false;
    }
    bool Kind(uintptr_t p, std::string_view name)
    {
        ++Stats.KindCalls;
        constexpr std::array<std::string_view, 4> names{"NiAVObject", "BSFlattenedBoneTree", "BSGeometry", "NiNode"};
        const auto requested = std::find(names.begin(), names.end(), name);
        if (requested == names.end()) return false;
        const uint8_t wanted = 1u << (requested - names.begin());
        uintptr_t table{}, function{};
        if (!Read(p, table) || table > UINTPTR_MAX - 2 * sizeof(void*) || !Read(table + 2 * sizeof(void*), function)) return false;
        if (!ExecutableOnce(function)) return false;
        // Do not assume GetRTTI ignores its object: dispatch for every request,
        // and include its returned pointer plus the current slot in the key.
        const auto rtti = Access.Rtti(p, function);
        const Key key{table, function, rtti};
        if (const auto found = Classes.find(key); found != Classes.end())
        {
            // The launcher maps engine headers AND type names writable. Check
            // every mutable operand before reuse, including name terminators;
            // both positive and negative class answers depend on these bytes.
            bool unchanged = true;
            for (const auto& guard : found->second.Headers)
            {
                ++Stats.KindHeaderChecks;
                std::array<uintptr_t, 2> header{};
                if (!Read(guard.Address, header) || header != guard.Value) { unchanged = false; break; }
            }
            if (unchanged) for (const auto& guard : found->second.Names)
            {
                ++Stats.KindNameChecks;
                const auto size = guard.Value.size() + 1;
                if (!Range(guard.Address, size) || std::memcmp(reinterpret_cast<const void*>(guard.Address), guard.Value.c_str(), size))
                { unchanged = false; break; }
            }
            if (unchanged)
            {
                ++Stats.KindCacheHits;
                return (found->second.Bits & wanted) != 0;
            }
            ++Stats.KindCacheInvalidations;
            Classes.erase(found);
        }
        Class entry;
        bool complete = true;
        auto current = rtti;
        size_t depth{};
        for (; current && depth < 32; ++depth)
        {
            uintptr_t text{}, parent{};
            std::string type;
            bool immutableName = true;
            if (current > UINTPTR_MAX - sizeof(uintptr_t) || !Read(current, text) || !Read(current + sizeof(uintptr_t), parent) ||
                !Name(text, type, &immutableName)) { complete = false; break; }
            if (!ReadOnly(current, 2 * sizeof(uintptr_t))) entry.Headers.push_back({current, {text, parent}});
            for (size_t i = 0; i < names.size(); ++i) if (type == names[i]) entry.Bits |= 1u << i;
            if (!immutableName) entry.Names.push_back({text, std::move(type)});
            current = parent;
        }
        // Partial/cyclic chains are never cached. Found classes still behave
        // as the old early-return walk did on this read.
        if (current) complete = false;
        const auto bits = entry.Bits;
        if (complete && Classes.size() < 256) Classes.emplace(key, std::move(entry));
        return (bits & wanted) != 0;
    }
private:
    struct Key
    {
        uintptr_t Table, Function, Rtti;
        bool operator==(const Key&) const = default;
    };
    struct Hash
    {
        size_t operator()(const Key& key) const { return (key.Table * 1099511628211ULL ^ key.Function) * 1099511628211ULL ^ key.Rtti; }
    };
    struct Header { uintptr_t Address; std::array<uintptr_t, 2> Value; };
    struct NameGuard { uintptr_t Address; std::string Value; };
    struct Class { uint8_t Bits{}; std::vector<Header> Headers; std::vector<NameGuard> Names; };
    std::unordered_map<Key, Class, Hash> Classes;
    std::unordered_map<uintptr_t, bool> Executables; // one backend question per distinct slot function
    size_t Last{SIZE_MAX};

    // The interval holding p, or null. Binary search with a last-hit hint; never probes.
    const BodyMemoryInterval* Lookup(uintptr_t p)
    {
        if (Last < Intervals.size() && p >= Intervals[Last].Begin && p < Intervals[Last].End)
        {
            ++Stats.LastRegionHits;
            return &Intervals[Last];
        }
        auto it = std::upper_bound(Intervals.begin(), Intervals.end(), p,
            [](uintptr_t value, const BodyMemoryInterval& interval) { return value < interval.Begin; });
        if (it == Intervals.begin()) return nullptr;
        --it;
        if (p < it->End) { Last = static_cast<size_t>(it - Intervals.begin()); return &*it; }
        return nullptr;
    }
    // Every byte of [p, end) is inside a proven interval, probing the uncovered parts.
    bool Cover(uintptr_t p, uintptr_t end)
    {
        if (!p || end < p) return false;
        while (p < end)
        {
            if (const auto* hit = Lookup(p)) { p = hit->End; continue; }
            if (Intervals.size() >= kIntervalCap) { ++Stats.IntervalCapHits; return false; }
            BodyMemoryInterval fresh{};
            ++Stats.Probes;
            if (!Access.Probe(p, end - p, fresh, Stats) || fresh.Begin > p || fresh.End <= p) return false;
            Insert(fresh);
            p = fresh.End;
        }
        return true;
    }
    void Insert(BodyMemoryInterval fresh)
    {
        const bool merge = Access.MergeIntervals();
        // Merging absorbs an interval that ends exactly at fresh.Begin; otherwise fresh goes after it.
        auto first = std::lower_bound(Intervals.begin(), Intervals.end(), fresh.Begin,
            [merge](const BodyMemoryInterval& interval, uintptr_t value) { return merge ? interval.End < value : interval.End <= value; });
        if (merge)
        {
            auto last = first;
            while (last != Intervals.end() && last->Begin <= fresh.End)
            {
                fresh.Begin = std::min(fresh.Begin, last->Begin);
                fresh.End = std::max(fresh.End, last->End);
                ++last;
            }
            first = Intervals.erase(first, last);
        }
        Last = static_cast<size_t>(Intervals.insert(first, fresh) - Intervals.begin());
        Stats.Intervals = static_cast<uint32_t>(Intervals.size());
        if (!Access.MergeIntervals()) Stats.Regions = Stats.Intervals;
    }
    bool ExecutableOnce(uintptr_t function)
    {
        if (const auto found = Executables.find(function); found != Executables.end()) return found->second;
        const bool executable = Access.Executable(function, Stats);
        Executables.emplace(function, executable);
        return executable;
    }
    bool ReadOnly(uintptr_t p, size_t bytes)
    {
        if (bytes > UINTPTR_MAX - p) return false;
        return Cover(p, p + bytes) && Access.KnownReadOnly(p, bytes);
    }
};
}
