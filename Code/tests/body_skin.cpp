#include <catch2/catch.hpp>
#include "../client/Games/Skyrim/NetImmerse/BodySkinReader.h"
#include <array>
#include <cstring>
#include <vector>

using namespace BodyTracking;
namespace
{
struct SkinFixture
{
    std::array<std::byte, 0x70> Skin{}, Data{};
    std::array<uintptr_t, 2> Bones{0x1000, 0}, Worlds{0x107c, 0x2034};
    struct Check { uintptr_t Address; std::vector<std::byte> Bytes; };
    std::vector<Check> Checks;
    SkinReadStats Stats;
    SkinReadDiagnostic Diagnostic;
    uintptr_t Unreadable{};
    uint32_t MapCalls{};

    template<class T> void Set(std::array<std::byte, 0x70>& object, size_t at, T value)
    {
        std::memcpy(object.data() + at, &value, sizeof(value));
    }
    SkinFixture()
    {
        Set(Skin, 0x10, reinterpret_cast<uintptr_t>(Data.data()));
        Set(Skin, 0x28, reinterpret_cast<uintptr_t>(Bones.data()));
        Set(Skin, 0x30, reinterpret_cast<uintptr_t>(Worlds.data()));
        Set(Data, 0x58, uint32_t{2});
        // +0x3C deliberately remains zero: linked inputs, no render yet.
    }
    bool Range(uintptr_t address, size_t bytes) const
    {
        if (bytes > UINTPTR_MAX - address) return false;
        for (auto [begin, size] : std::array<std::pair<uintptr_t, size_t>, 4>{{
                 {reinterpret_cast<uintptr_t>(Skin.data()), sizeof(Skin)},
                 {reinterpret_cast<uintptr_t>(Data.data()), sizeof(Data)},
                 {reinterpret_cast<uintptr_t>(Bones.data()), sizeof(Bones)},
                 {reinterpret_cast<uintptr_t>(Worlds.data()), sizeof(Worlds)}}})
            if (address >= begin && address + bytes <= begin + size) return true;
        return false;
    }
    template<class T> bool Guard(uintptr_t address, T& value)
    {
        if (address == Unreadable || !Range(address, sizeof(value))) return false;
        std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(value));
        Check check{address, std::vector<std::byte>(sizeof(value))};
        std::memcpy(check.Bytes.data(), &value, sizeof(value));
        Checks.push_back(std::move(check));
        return true;
    }
    bool Stable() const
    {
        for (const auto& check : Checks)
            if (std::memcmp(reinterpret_cast<void*>(check.Address), check.Bytes.data(), check.Bytes.size())) return false;
        return true;
    }
    bool Read()
    {
        return ReadSkinConsumers(reinterpret_cast<uintptr_t>(Skin.data()),
            [&](uintptr_t address, auto& value) { return Guard(address, value); },
            [&](uintptr_t address, size_t bytes) { return Range(address, bytes); },
            [&](uintptr_t bone, uintptr_t world) -> const char*
            {
                ++MapCalls;
                if (world != 0x107c && world != 0x2034) return "unmapped-world";
                if (bone && bone != (world == 0x107c ? 0x1000 : 0)) return "bone-alias-mismatch";
                return nullptr;
            }, Stats, Diagnostic);
    }
};
}

TEST_CASE("Skin reader uses engine input count before first render and preserves flattened null-bone aliases", "[body][skin]")
{
    SkinFixture f;
    REQUIRE(f.Read());
    REQUIRE(f.Diagnostic.Count == 2);
    REQUIRE(f.Stats.MappedSlots == 2);
    REQUIRE(f.Stable());
    for (const auto& check : f.Checks)
        REQUIRE(check.Address != reinterpret_cast<uintptr_t>(f.Skin.data()) + 0x3c);
    f.Set(f.Skin, 0x3c, uint32_t{99}); // output changes are not input generations
    REQUIRE(f.Stable());
    f.Set(f.Data, 0x58, uint32_t{1});
    REQUIRE_FALSE(f.Stable());
}

TEST_CASE("Skin reader distinguishes empty and unlinked input from missing bones on a real consumer", "[body][skin]")
{
    SkinFixture f;
    SECTION("empty count still guarded")
    {
        f.Set(f.Data, 0x58, uint32_t{0});
        REQUIRE(f.Read()); REQUIRE(f.Stats.Empty == 1); REQUIRE(f.MapCalls == 0);
        f.Set(f.Data, 0x58, uint32_t{2}); REQUIRE_FALSE(f.Stable());
    }
    SECTION("unlinked worlds still guarded")
    {
        f.Set(f.Skin, 0x30, uintptr_t{0});
        REQUIRE(f.Read()); REQUIRE(f.Stats.Unlinked == 1); REQUIRE(f.MapCalls == 0);
        f.Set(f.Skin, 0x30, reinterpret_cast<uintptr_t>(f.Worlds.data())); REQUIRE_FALSE(f.Stable());
    }
    SECTION("populated worlds with missing bones must refuse")
    {
        f.Set(f.Skin, 0x28, uintptr_t{0});
        REQUIRE_FALSE(f.Read()); REQUIRE(std::string(f.Diagnostic.Operand) == "skin-bones");
        REQUIRE(f.Stats.Unlinked == 0);
    }
    SECTION("individual null world slot is not a consumer")
    {
        f.Worlds[1] = 0;
        REQUIRE(f.Read()); REQUIRE(f.Stats.NullSlots == 1); REQUIRE(f.Stats.MappedSlots == 1);
        f.Worlds[1] = 0x2034; REQUIRE_FALSE(f.Stable());
    }
}

TEST_CASE("Skin reader retains count, address, read and non-null consumer refusals", "[body][skin]")
{
    SkinFixture f;
    const char* expected{};
    bool unreadable{};
    SECTION("missing data") { f.Set(f.Skin, 0x10, uintptr_t{0}); expected = "skin-data"; }
    SECTION("overflowing data") { f.Set(f.Skin, 0x10, uintptr_t{UINTPTR_MAX}); expected = "skin-data-address"; }
    SECTION("unreadable count") { f.Unreadable = reinterpret_cast<uintptr_t>(f.Data.data()) + 0x58; expected = "skin-count-read"; unreadable = true; }
    SECTION("excessive count") { f.Set(f.Data, 0x58, uint32_t{1025}); expected = "skin-count"; }
    SECTION("short bones array") { f.Set(f.Data, 0x58, uint32_t{3}); expected = "skin-bones-range"; unreadable = true; }
    SECTION("unreadable worlds") { f.Set(f.Skin, 0x30, uintptr_t{1}); expected = "skin-worlds-range"; unreadable = true; }
    SECTION("unknown non-null alias") { f.Worlds[1] = 0x9999; expected = "unmapped-world"; }
    SECTION("mismatched bone alias") { f.Bones[1] = 0x1000; expected = "bone-alias-mismatch"; }
    SECTION("unreadable partition") { f.Unreadable = reinterpret_cast<uintptr_t>(f.Skin.data()) + 0x18; expected = "skin-partition-read"; unreadable = true; }
    REQUIRE(expected);
    REQUIRE_FALSE(f.Read());
    REQUIRE(std::string(f.Diagnostic.Operand) == expected);
    REQUIRE(f.Diagnostic.Unreadable == unreadable);
}

TEST_CASE("Skin address overflow refuses before any memory access", "[body][skin]")
{
    SkinFixture f;
    REQUIRE_FALSE(ReadSkinConsumers(UINTPTR_MAX,
        [&](uintptr_t, auto&) { FAIL("overflow must not read memory"); return false; },
        [&](uintptr_t, size_t) { FAIL("overflow must not query ranges"); return false; },
        [&](uintptr_t, uintptr_t) -> const char* { FAIL("overflow must not map"); return nullptr; },
        f.Stats, f.Diagnostic));
    REQUIRE(std::string(f.Diagnostic.Operand) == "skin-address");
    REQUIRE_FALSE(f.Diagnostic.Unreadable);
}
