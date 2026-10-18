#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

/**
 * @brief The schedule for redrawing a remote player body's worn armor after its inventory is set.
 *
 * A remote body built while its owner wears no torso armor shows none of its worn armor: the hood, boots and
 * gauntlets are in its worn list, and the owner sees them equipped, but they are not drawn. Feet and hands
 * vanish with the boots and gauntlets. A body with a torso piece draws everything. Any armor change by the
 * owner redraws the whole body at once, so the owner taking one piece off is a complete repair, and that is
 * the shape this follows: take the worn armor off, wait, put it back.
 *
 * The two halves run in separate updates, at least kRestoreGap apart, because an unequip and an equip queued
 * into the same drain cancel out (see ReattachHandItems). What goes back on is what was worn when the armor
 * came off, less anything the owner changed in between: their change has already been applied by the time the
 * restore runs, and putting a removed piece back, or one whose slots the owner has since filled, would undo it.
 *
 * Kept free of game types so the schedule can be tested on its own.
 */
struct GearRedraw
{
    // After the first weapon state pass, which is also at half a second.
    static constexpr double kStripAt = 0.5;
    static constexpr double kRestoreGap = 0.25;

    enum class Step : uint8_t
    {
        Wait,
        Strip,
        Restore,
    };

    // An armor piece taken off, and the biped slot mask it occupies (TESObjectARMO::slotType).
    struct Piece
    {
        uint32_t Id = 0;
        uint32_t Slots = 0;
    };

    GearRedraw() = default;
    GearRedraw(const uint32_t aEntity, const uint32_t aOwnershipEpoch) noexcept
        : Entity(aEntity)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    /**
     * @brief Adds the frame time and says what is due. One step per call, so a long frame cannot run both halves
     *        in one update.
     */
    Step Advance(const double acDelta) noexcept
    {
        Timer += acDelta;

        if (!Stripped)
            return Timer >= kStripAt ? Step::Strip : Step::Wait;

        return Timer - StrippedAt >= kRestoreGap ? Step::Restore : Step::Wait;
    }

    void MarkStripped(std::vector<Piece> aArmor) noexcept
    {
        Stripped = true;
        StrippedAt = Timer;
        Armor = std::move(aArmor);
    }

    /**
     * @brief Drops an item the owner equipped or unequipped while the armor was off.
     *
     * Before the strip there is nothing to drop: the strip takes whatever is worn at that moment, which already
     * includes the change.
     */
    void Forget(const uint32_t acItemId) noexcept
    {
        if (Stripped)
            Armor.erase(std::remove_if(Armor.begin(), Armor.end(), [acItemId](const Piece& acPiece) { return acPiece.Id == acItemId; }), Armor.end());
    }

    /**
     * @brief The pieces to put back, given the slots the body wears now.
     *
     * A piece whose slots are now taken was replaced by the owner while it was off (boots A came off, the owner
     * put on boots B), so it goes into aSkipped instead: equipping it would take B off again.
     */
    std::vector<uint32_t> Restorable(const uint32_t acWornSlots, std::vector<uint32_t>& aSkipped) const noexcept
    {
        std::vector<uint32_t> restore;
        for (const Piece& cPiece : Armor)
        {
            if (cPiece.Slots & acWornSlots)
                aSkipped.push_back(cPiece.Id);
            else
                restore.push_back(cPiece.Id);
        }

        return restore;
    }

    // The hand item passes strip and restore the same armor, so they wait while it is off.
    bool HoldsHandPasses() const noexcept { return Stripped; }

    uint32_t Entity = 0;
    uint32_t OwnershipEpoch = 0;
    double Timer = 0.0;
    double StrippedAt = 0.0;
    bool Stripped = false;
    std::vector<Piece> Armor{};
};
