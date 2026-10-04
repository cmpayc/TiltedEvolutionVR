#include <catch2/catch.hpp>
#include "../client/Games/Skyrim/NetImmerse/BodyReadMemory.h"

using namespace BodyTracking;
namespace
{
// Exact regions with explicit permissions, no page rounding and no merging: the template's logic is
// under test here; the production backends are tested against real pages in body_probe.cpp.
struct TestMemory
{
    std::vector<BodyMemoryRegion> Map;
    std::unordered_map<uintptr_t, uintptr_t> Types;
    uint32_t Dispatches{}, ExecutableCalls{};
    const BodyMemoryRegion* Find(uintptr_t p) const
    {
        for (const auto& region : Map) if (p >= region.Begin && p < region.End) return &region;
        return nullptr;
    }
    bool MergeIntervals() const { return false; }
    bool Probe(uintptr_t p, size_t, BodyMemoryInterval& out, NativeReadStats&)
    {
        const auto* region = Find(p);
        if (!region) return false;
        out = {region->Begin, region->End};
        return true;
    }
    bool Writable(uintptr_t p, uintptr_t end, NativeReadStats&) const
    {
        while (p < end)
        {
            const auto* region = Find(p);
            if (!region || !region->Write) return false;
            p = region->End;
        }
        return true;
    }
    bool Executable(uintptr_t p, NativeReadStats&)
    {
        ++ExecutableCalls;
        const auto* region = Find(p);
        return region && region->Execute;
    }
    bool KnownReadOnly(uintptr_t p, size_t bytes) const
    {
        const auto end = p + bytes;
        while (p < end)
        {
            const auto* region = Find(p);
            if (!region || region->Write) return false;
            p = region->End;
        }
        return true;
    }
    uintptr_t Rtti(uintptr_t p, uintptr_t) { ++Dispatches; return Types.at(p); }
};
using Reader = BodyReadMemory<TestMemory>;
void Map(Reader& reader, const void* data, size_t size, bool writable = false, bool executable = false)
{
    const auto begin = reinterpret_cast<uintptr_t>(data);
    reader.Access.Map.push_back({begin, begin + size, writable, executable});
}
struct Types
{
    std::array<uintptr_t, 2> Objects{};
    std::array<uintptr_t, 3> Table{};
    char Code{};
    std::array<std::array<uintptr_t, 2>, 2> Rtti{};
    std::array<std::array<char, 128>, 2> Names{};
    Reader Make(bool mutableRtti = false)
    {
        std::memcpy(Names[0].data(), "NiNode", sizeof("NiNode"));
        std::memcpy(Names[1].data(), "NiAVObject", sizeof("NiAVObject"));
        Rtti[0] = {reinterpret_cast<uintptr_t>(Names[0].data()), reinterpret_cast<uintptr_t>(Rtti[1].data())};
        Rtti[1] = {reinterpret_cast<uintptr_t>(Names[1].data()), 0};
        Table[2] = reinterpret_cast<uintptr_t>(&Code);
        Objects.fill(reinterpret_cast<uintptr_t>(Table.data()));
        Reader reader;
        Map(reader, Objects.data(), sizeof(Objects), true);
        Map(reader, Table.data(), sizeof(Table));
        Map(reader, &Code, sizeof(Code), false, true);
        Map(reader, Rtti.data(), sizeof(Rtti), mutableRtti);
        Map(reader, Names.data(), sizeof(Names));
        for (auto& object : Objects) reader.Access.Types[reinterpret_cast<uintptr_t>(&object)] = reinterpret_cast<uintptr_t>(Rtti[0].data());
        return reader;
    }
};
}

TEST_CASE("Native memory names respect region edges, permissions and the byte limit", "[body][memory]")
{
    std::array<char, 256> text{};
    std::memcpy(text.data()+14, "ABC", 4);
    Reader reader;
    Map(reader, text.data(), 16);
    Map(reader, text.data()+16, 240, true);
    std::string name;
    REQUIRE(reader.Name(reinterpret_cast<uintptr_t>(text.data()+14), name));
    REQUIRE(name == "ABC");
    REQUIRE(reader.Stats.Probes == 2);
    REQUIRE(reader.Stats.VirtualQueries == 0);
    REQUIRE(reader.Stats.Intervals == 2);
    REQUIRE(reader.Stats.NameChunks == 2);
    REQUIRE_FALSE(reader.Range(reinterpret_cast<uintptr_t>(text.data()+14), 4, true));
    REQUIRE(reader.Range(reinterpret_cast<uintptr_t>(text.data()+16), 4, true));
    REQUIRE_FALSE(reader.Range(UINTPTR_MAX-2, 4));
    Reader edge;
    Map(edge, text.data(), 16);
    REQUIRE_FALSE(edge.Name(reinterpret_cast<uintptr_t>(text.data()+14), name));
    text[15] = 0;
    REQUIRE(edge.Name(reinterpret_cast<uintptr_t>(text.data()+14), name));
    REQUIRE(name == "A"); // terminator before inaccessible next region
    text.fill('X');
    Reader longName;
    Map(longName, text.data(), text.size());
    REQUIRE_FALSE(longName.Name(reinterpret_cast<uintptr_t>(text.data()), name));
    text[127] = 0;
    REQUIRE(longName.Name(reinterpret_cast<uintptr_t>(text.data()), name));
    REQUIRE(name.size() == 127);
    REQUIRE(longName.Name(0, name));
    REQUIRE(name.empty());
}

TEST_CASE("Native class cache reduces queries but dispatches each object and rejects nonexecutable code", "[body][memory]")
{
    Types types;
    auto reader = types.Make();
    for (size_t repeat = 0; repeat < 20; ++repeat)
        for (auto& object : types.Objects)
        {
            const auto p = reinterpret_cast<uintptr_t>(&object);
            REQUIRE(reader.Kind(p, "NiAVObject"));
            REQUIRE(reader.Kind(p, "NiNode"));
            REQUIRE_FALSE(reader.Kind(p, "BSGeometry"));
            REQUIRE_FALSE(reader.Kind(p, "BSFlattenedBoneTree"));
        }
    REQUIRE(reader.Stats.Probes == 4); // objects, table, rtti, names; the code page is answered by Executable
    REQUIRE(reader.Access.ExecutableCalls == 1); // once per distinct slot function, not per Kind call
    REQUIRE(reader.Stats.KindCacheHits == 159);
    REQUIRE(reader.Access.Dispatches == 160);
    // Same vtable, different object-specific RTTI result: no false reuse.
    const auto second = reinterpret_cast<uintptr_t>(&types.Objects[1]);
    reader.Access.Types[second] = reinterpret_cast<uintptr_t>(types.Rtti[1].data());
    REQUIRE_FALSE(reader.Kind(second, "NiNode"));
    REQUIRE(reader.Kind(second, "NiAVObject"));
    const auto calls = reader.Access.Dispatches;
    types.Table[2] = reinterpret_cast<uintptr_t>(types.Names.data());
    REQUIRE_FALSE(reader.Kind(second, "NiAVObject"));
    REQUIRE(reader.Access.Dispatches == calls);
    auto denied = types.Make();
    denied.Access.Map[2].Execute = false;
    REQUIRE_FALSE(denied.Kind(reinterpret_cast<uintptr_t>(&types.Objects[0]), "NiAVObject"));
    REQUIRE(denied.Access.Dispatches == 0);
}

TEST_CASE("Mutable and incomplete RTTI chains cannot create stale class answers", "[body][memory]")
{
    Types types;
    auto reader = types.Make(true);
    const auto p = reinterpret_cast<uintptr_t>(&types.Objects[0]);
    REQUIRE(reader.Kind(p, "NiAVObject"));
    types.Rtti[0][1] = 0;
    REQUIRE_FALSE(reader.Kind(p, "NiAVObject"));
    REQUIRE(reader.Kind(p, "NiNode"));
    REQUIRE(reader.Stats.KindCacheHits == 1);
    REQUIRE(reader.Stats.KindCacheInvalidations == 1);
    types.Rtti[0][1] = 1; // later invalid parent must not erase an earlier match
    REQUIRE(reader.Kind(p, "NiNode"));
    REQUIRE_FALSE(reader.Kind(p, "NiAVObject"));
    REQUIRE(reader.Stats.KindCacheHits == 1);
    REQUIRE(reader.Stats.KindCacheInvalidations == 2);
}

TEST_CASE("Writable RTTI headers and names reuse class answers only after exact validation", "[body][memory]")
{
    Types types;
    auto reader = types.Make(true);
    const bool mutableNames = GENERATE(false, true);
    reader.Access.Map.back().Write = mutableNames;
    const auto p = reinterpret_cast<uintptr_t>(&types.Objects[0]);
    for (size_t repeat = 0; repeat < 20; ++repeat)
        for (auto& object : types.Objects)
        {
            const auto node = reinterpret_cast<uintptr_t>(&object);
            REQUIRE(reader.Kind(node, "NiNode"));
            REQUIRE(reader.Kind(node, "NiAVObject"));
            REQUIRE_FALSE(reader.Kind(node, "BSGeometry"));
            REQUIRE_FALSE(reader.Kind(node, "BSFlattenedBoneTree"));
        }
    REQUIRE(reader.Access.Dispatches == 160);
    REQUIRE(reader.Stats.KindCacheHits == 159);
    REQUIRE(reader.Stats.KindHeaderChecks == 318);
    REQUIRE(reader.Stats.KindNameChecks == (mutableNames ? 318 : 0));
    REQUIRE(reader.Stats.KindCacheInvalidations == 0);
    REQUIRE(reader.Stats.NameChunks == 2); // the uncached walk reads 320
    REQUIRE(reader.Stats.Probes == 4);

    // Mutate an ancestor's name pointer without changing the object, vtable,
    // GetRTTI slot or leaf RTTI. Neither a positive nor negative may be stale.
    types.Rtti[1][0] = reinterpret_cast<uintptr_t>(types.Names[0].data());
    REQUIRE_FALSE(reader.Kind(p, "NiAVObject"));
    REQUIRE(reader.Kind(p, "NiNode"));
    REQUIRE(reader.Stats.KindCacheInvalidations == 1);
    types.Rtti[1][0] = reinterpret_cast<uintptr_t>(types.Names[1].data());
    REQUIRE(reader.Kind(p, "NiAVObject"));
    REQUIRE(reader.Stats.KindCacheInvalidations == 2);

    // A cycle is a partial chain; repeated reads must not cache its negatives.
    types.Rtti[1][1] = reinterpret_cast<uintptr_t>(types.Rtti[0].data());
    const auto hits = reader.Stats.KindCacheHits;
    REQUIRE_FALSE(reader.Kind(p, "BSGeometry"));
    REQUIRE(reader.Kind(p, "NiNode"));
    REQUIRE(reader.Stats.KindCacheHits == hits);
    REQUIRE(reader.Stats.KindCacheInvalidations == 3);
}

TEST_CASE("Mutable RTTI name bytes and terminators are checked and caches end with the invocation", "[body][memory]")
{
    Types types;
    auto reader = types.Make(true);
    reader.Access.Map.back().Write = true;
    const auto p = reinterpret_cast<uintptr_t>(&types.Objects[0]);
    REQUIRE(reader.Kind(p, "NiNode"));
    std::memcpy(types.Names[0].data(), "BSGeometry", sizeof("BSGeometry"));
    REQUIRE_FALSE(reader.Kind(p, "NiNode"));
    REQUIRE(reader.Kind(p, "BSGeometry"));
    REQUIRE(reader.Stats.KindCacheHits == 1);
    REQUIRE(reader.Stats.KindCacheInvalidations == 1);
    REQUIRE(reader.Stats.KindNameChecks > 0);
    // Extending the name by overwriting its terminator must invalidate it.
    types.Names[0][10] = 'X';
    REQUIRE_FALSE(reader.Kind(p, "BSGeometry"));
    REQUIRE(reader.Stats.KindCacheInvalidations == 2);
    types.Names[0][10] = 0;
    REQUIRE(reader.Kind(p, "BSGeometry"));
    // A writable ancestor's contents matter even when its pointer is unchanged.
    std::memcpy(types.Names[1].data(), "Other", sizeof("Other"));
    REQUIRE_FALSE(reader.Kind(p, "NiAVObject"));
    REQUIRE(reader.Kind(p, "BSGeometry"));

    auto first = types.Make(true);
    REQUIRE(first.Kind(p, "NiNode"));
    REQUIRE(first.Kind(p, "NiNode"));
    REQUIRE(first.Stats.KindCacheHits == 1);
    Reader next;
    next.Access = first.Access;
    next.Access.Dispatches = 0;
    REQUIRE(next.Kind(p, "NiNode"));
    REQUIRE(next.Stats.KindCacheHits == 0);
    REQUIRE(next.Stats.NameChunks == 2);
    REQUIRE(next.Access.Dispatches == 1);
}

TEST_CASE("Interval cap refuses the next probe and counts it", "[body][memory]")
{
    std::vector<char> bytes(Reader::kIntervalCap + 2, 'x');
    Reader reader;
    for (size_t i = 0; i < bytes.size(); ++i) Map(reader, bytes.data() + i, 1);
    for (size_t i = 0; i < Reader::kIntervalCap; ++i) REQUIRE(reader.Range(reinterpret_cast<uintptr_t>(bytes.data() + i), 1));
    REQUIRE(reader.Stats.Intervals == Reader::kIntervalCap);
    REQUIRE(reader.Stats.IntervalCapHits == 0);
    REQUIRE_FALSE(reader.Range(reinterpret_cast<uintptr_t>(bytes.data() + Reader::kIntervalCap), 1));
    REQUIRE(reader.Stats.IntervalCapHits == 1);
    REQUIRE(reader.Range(reinterpret_cast<uintptr_t>(bytes.data()), 1)); // cached intervals still answer
    REQUIRE(reader.Stats.IntervalCapHits == 1);
}

TEST_CASE("Write permission is asked of the backend for the exact span and executability once per slot", "[body][memory]")
{
    std::array<char, 64> text{};
    Reader reader;
    Map(reader, text.data(), 32);
    Map(reader, text.data() + 32, 32, true);
    REQUIRE(reader.Range(reinterpret_cast<uintptr_t>(text.data() + 30), 4));
    REQUIRE_FALSE(reader.Range(reinterpret_cast<uintptr_t>(text.data() + 30), 4, true));
    REQUIRE(reader.Range(reinterpret_cast<uintptr_t>(text.data() + 32), 4, true));
    REQUIRE(reader.Stats.Probes == 2);
    REQUIRE(reader.Stats.Intervals == 2); // the fixture never merges
}
