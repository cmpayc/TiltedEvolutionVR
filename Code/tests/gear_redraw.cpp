#include <catch2/catch.hpp>
#include "../client/Services/GearRedraw.h"

namespace
{
// One VR frame at 90 Hz, and the hood and boots worn in the reproducing run (Skyrim.esm 10DD3A, 13ED7).
constexpr double kFrame = 1.0 / 90.0;
constexpr uint32_t kHood = 0x10DD3A;
constexpr uint32_t kBoots = 0x13ED7;
// Their biped slot masks as decoded from Skyrim.esm (BOD2): hair and circlet, feet.
constexpr uint32_t kHoodSlots = 0x1002;
constexpr uint32_t kBootsSlots = 0x80;
// Another pair of boots in the same slot (Skyrim.esm 12E4B, iron boots, which the owner equipped in the run).
constexpr uint32_t kOtherBoots = 0x12E4B;

GearRedraw::Step RunUntilNotWaiting(GearRedraw& aRedraw, int& aFrames)
{
    for (aFrames = 1; aFrames < 1000; ++aFrames)
    {
        const GearRedraw::Step cStep = aRedraw.Advance(kFrame);
        if (cStep != GearRedraw::Step::Wait)
            return cStep;
    }

    return GearRedraw::Step::Wait;
}
} // namespace

TEST_CASE("Gear redraw strips at half a second and restores a quarter second later", "[gear_redraw]")
{
    GearRedraw redraw(7, 3);

    int frames = 0;
    REQUIRE(RunUntilNotWaiting(redraw, frames) == GearRedraw::Step::Strip);
    CHECK(redraw.Timer >= GearRedraw::kStripAt);
    CHECK(redraw.Timer < GearRedraw::kStripAt + 2 * kFrame);
    CHECK_FALSE(redraw.HoldsHandPasses());

    redraw.MarkStripped({{kBoots, kBootsSlots}, {kHood, kHoodSlots}});
    CHECK(redraw.HoldsHandPasses());

    REQUIRE(RunUntilNotWaiting(redraw, frames) == GearRedraw::Step::Restore);
    CHECK(redraw.Timer - redraw.StrippedAt >= GearRedraw::kRestoreGap);
    CHECK(frames > 1);
}

TEST_CASE("Gear redraw never runs both halves in one update, however long the frame", "[gear_redraw]")
{
    GearRedraw redraw(7, 3);

    REQUIRE(redraw.Advance(5.0) == GearRedraw::Step::Strip);
    redraw.MarkStripped({{kHood, kHoodSlots}});

    // The restore is due only on a later update, measured from the strip.
    CHECK(redraw.Advance(0.0) == GearRedraw::Step::Wait);
    CHECK(redraw.Advance(5.0) == GearRedraw::Step::Restore);
}

TEST_CASE("Gear redraw leaves out what the owner changed while the armor was off", "[gear_redraw]")
{
    GearRedraw redraw(7, 3);

    // Before the strip there is nothing to drop: the strip reads what is worn then.
    redraw.Forget(kBoots);
    REQUIRE(redraw.Advance(1.0) == GearRedraw::Step::Strip);
    redraw.MarkStripped({{kBoots, kBootsSlots}, {kHood, kHoodSlots}});

    redraw.Forget(kBoots);
    REQUIRE(redraw.Armor.size() == 1);
    CHECK(redraw.Armor[0].Id == kHood);

    // An item that was never off is ignored.
    redraw.Forget(0x12EB7);
    CHECK(redraw.Armor.size() == 1);
}

TEST_CASE("Gear redraw keeps the entity and owner it was queued for", "[gear_redraw]")
{
    const GearRedraw redraw(0x1B, 4);

    CHECK(redraw.Entity == 0x1B);
    CHECK(redraw.OwnershipEpoch == 4);
    CHECK_FALSE(redraw.Stripped);
    CHECK(redraw.Armor.empty());
}

TEST_CASE("Gear redraw does not put back a piece whose slot the owner filled while it was off", "[gear_redraw]")
{
    GearRedraw redraw(7, 3);
    REQUIRE(redraw.Advance(1.0) == GearRedraw::Step::Strip);
    redraw.MarkStripped({{kBoots, kBootsSlots}, {kHood, kHoodSlots}});

    // The owner put on other boots in between: their notification equipped them and dropped nothing of ours.
    redraw.Forget(kOtherBoots);
    REQUIRE(redraw.Armor.size() == 2);

    std::vector<uint32_t> skipped;
    const std::vector<uint32_t> cRestore = redraw.Restorable(kBootsSlots, skipped);

    REQUIRE(cRestore.size() == 1);
    CHECK(cRestore[0] == kHood);
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0] == kBoots);

    // Nothing worn now: everything goes back.
    skipped.clear();
    CHECK(redraw.Restorable(0, skipped).size() == 2);
    CHECK(skipped.empty());
}
