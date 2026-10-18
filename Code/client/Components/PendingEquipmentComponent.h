#pragma once

#include <Structs/Inventory.h>

/**
 * @brief The state a remote player's body had when we lost sight of it, to be put back afterwards.
 *
 * A remote player's body only ever has this applied at spawn and then by incoming change notifications.
 * Nothing re-applies it, which did not matter while losing sight of the body deleted it and the next spawn
 * built a fresh one.
 *
 * Now that the body is kept, every gap of that kind shows. Measured on 2026-08-19: three worn armour pieces
 * before the body lost its 3D, one after it came back, and the hand sync probe found 24 bones below the
 * shoulder where a freshly spawned body has 39, the difference being the nodes armour and weapons add. So the
 * game does not restore this when it rebuilds the body, and the state is genuinely gone rather than merely
 * unrendered.
 *
 * Inventory::Entry carries ExtraWorn and ExtraWornLeft, so a whole inventory captured at the right moment is
 * enough to put the equipment back, and SetActorInventory is the same call the spawn path already uses.
 */
struct PendingEquipmentComponent
{
    Inventory Content{};

    /**
     * @brief Whether the weapon was out, restored through CharacterService::m_weaponDrawUpdates.
     *
     * Deliberately re-queued rather than applied directly. That queue exists because, as the comment on
     * ApplyCachedWeaponDraws puts it, Skyrim's weapon drawing is the most finnicky thing in existence: it
     * applies once after half a second and again after two, because a single attempt does not reliably take.
     * A rebuilt body needs that same treatment.
     */
    bool WeaponDrawn{false};

    /**
     * @brief Whether the body was crouching, recorded for diagnosis rather than restored.
     *
     * There is no sneak field on the wire. Crouch reaches a remote body only as animation graph variables
     * (kIsSneaking, kiIsInSneak in AnimationGraphDescriptor_Master_Behavior), so ActorState's own sneak bit on
     * a remote body is whatever the rebuild left. Logging both sides of the rebuild says whether that bit
     * survives, which decides whether crouch needs a wire field of its own.
     */
    bool Sneaking{false};
};
