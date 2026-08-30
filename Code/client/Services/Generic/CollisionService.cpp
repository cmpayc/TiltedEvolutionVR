#include <TiltedOnlinePCH.h>

#include <Services/CollisionService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#if TP_SKYRIMVR

#include <World.h>

#include <Services/TransportService.h>

#include <Components.h>

#include <AI/AIProcess.h>
#include <Actor.h>
#include <Misc/MiddleProcess.h>
#include <NetImmerse/NiNode.h>
#include <PlayerCharacter.h>
#include <Forms/TESForm.h>

namespace
{
bool IsReadable(const void* apPtr, const size_t aSize) noexcept
{
    if (!apPtr)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apPtr, &info, sizeof(info)))
        return false;

    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cReadable))
        return false;

    const auto cRegionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;

    return reinterpret_cast<uintptr_t>(apPtr) + aSize <= cRegionEnd;
}

/**
 * @brief The decorated type name of a candidate pointer, by reading MSVC's RTTI. Null if it is not an object.
 *
 * Reads only. The previous attempt asked the object for its type by calling vtable slot 2, which crashed the
 * session of 2026-08-23 17:09 with `call rax` on a candidate that was not an NiObject at all: readable memory
 * says nothing about what calling through it will do, and the type cannot be checked by a call that presumes
 * the type. Nothing here transfers control, so the worst a bad candidate costs is a failed check.
 *
 * The complete object locator sits at `vtable[-1]`. On x64 its fields past the signature are image relative,
 * and it stores its own RVA, so subtracting that from where it was found gives the base of whichever module
 * owns it without having to know which one that is. The base is then confirmed by its `MZ`, which makes a
 * false positive essentially impossible.
 */
const char* RttiName(const void* apCandidate) noexcept
{
    if (!IsReadable(apCandidate, sizeof(void*)))
        return nullptr;

    const auto* cpVtable = *reinterpret_cast<const uint8_t* const*>(apCandidate);

    if (!IsReadable(cpVtable - sizeof(void*), sizeof(void*)))
        return nullptr;

    const auto* cpLocator = *reinterpret_cast<const uint8_t* const*>(cpVtable - sizeof(void*));

    // signature, offset, cdOffset, type descriptor rva, class descriptor rva, self rva.
    constexpr size_t kLocatorSize = 0x18;

    if (!IsReadable(cpLocator, kLocatorSize))
        return nullptr;

    const auto cSignature = *reinterpret_cast<const uint32_t*>(cpLocator);
    if (cSignature != 1)
        return nullptr;

    const auto cTypeRva = *reinterpret_cast<const uint32_t*>(cpLocator + 0x0C);
    const auto cSelfRva = *reinterpret_cast<const uint32_t*>(cpLocator + 0x14);

    const auto cBase = reinterpret_cast<uintptr_t>(cpLocator) - cSelfRva;

    if (!IsReadable(reinterpret_cast<const void*>(cBase), 2) || *reinterpret_cast<const uint16_t*>(cBase) != 0x5A4D)
        return nullptr;

    // vftable pointer, spare, then the decorated name.
    constexpr size_t kNameOffset = 0x10;

    const auto* cpDescriptor = reinterpret_cast<const char*>(cBase + cTypeRva);

    if (!IsReadable(cpDescriptor, kNameOffset + 1))
        return nullptr;

    return cpDescriptor + kNameOffset;
}

// A decorated name reads ".?AVbhkCharacterController@@", so the wanted text is looked for inside it rather
// than matched whole. Bounded and checked a byte at a time, because it is still unverified memory.
bool RttiNameContains(const char* acpName, const char* acpWanted) noexcept
{
    constexpr size_t kMaxName = 128;

    for (size_t start = 0; start < kMaxName; ++start)
    {
        if (!IsReadable(acpName + start, 1) || !acpName[start])
            return false;

        size_t i = 0;

        while (acpWanted[i] && IsReadable(acpName + start + i, 1) && acpName[start + i] == acpWanted[i])
            ++i;

        if (!acpWanted[i])
            return true;
    }

    return false;
}

/**
 * @brief The loose things a character must not be able to shove about, and the layers a character's own
 *        collision sits on.
 *
 * Item drift is a character's collision being warped into a loose object, with havok resolving the overlap by
 * ejecting whatever it landed in. Rationing the warps trades the drift against being able to hit anyone, because
 * both come from that same contact. Removing the contact removes the drift instead of rationing it.
 *
 * Skyrim keeps one table for this, a bitfield per collision layer naming the layers it meets, so a single write
 * covers every actor in the game rather than each body as it spawns. That is also its limit. The table is per
 * layer and not per body, so the only way to let one character through a pair the table closes is to give that
 * character's bodies a layer of their own, which is what `s_mirrorLayers` is and what the local player gets.
 *
 * **A dropped weapon is not clutter.** It is L_WEAPON, the same layer a held one is on, so the pairs that keep a
 * body off a sword on the floor are the body layers against 5 and not against 4. Reported 2026-09-12: with
 * clutter settled, an unowned NPC brushing a sword on the ground still sent it drifting. That is why the clear
 * runs over both loose layers rather than over clutter alone.
 */
constexpr uint32_t kClutterLayer = 4;
constexpr uint32_t kWeaponLayer = 5;

/**
 * The layers, most wanted first, because the rows to mirror them onto can run out and what is left is a prefix
 * of this list. The character controller leads: it is the capsule a player walks around in and the only body
 * the local player turns out to have at all.
 *
 * **L_BIPED_NO_CC is the one that was missing**, and it is why clearing the other four stopped short. Its name
 * says what it is, biped collision on a body with no character controller, which is what the game puts an
 * actor's animated limbs on, and the layer table read on 2026-09-12 had it meeting clutter while 5, 8, 30 and
 * 32 no longer did. That is the contact that survived every earlier clear: a body nobody could make stop
 * pushing furniture, on both clients, which is also why the other client could watch a body shove items that
 * its own client said could not touch them.
 */
constexpr uint32_t kActorLayers[] = {30, 33, 8, 5, 32}; // controller, biped with no controller, biped, weapon, dead biped
constexpr size_t kActorLayerCount = sizeof(kActorLayers) / sizeof(kActorLayers[0]);
constexpr size_t kLayerCount = 64;

/**
 * The subset of those that are a character's actual body, which is every one of them except the weapon it is
 * holding. Only these are kept off clutter.
 *
 * The weapon is left out on purpose, because a held weapon and a dropped one are the same layer and there is no
 * telling them apart here. Keeping 5 off 5 would stop a body's sword ejecting a sword on the floor and would
 * also stop two swords on the floor from touching each other, which is a visible oddity in exchange for a
 * contact that only happens when somebody is swinging. A held weapon is still kept off clutter, as it has been
 * since the drift fix, by the pair spelled out in the patch.
 */
constexpr uint32_t kActorBodyLayers[] = {30, 33, 8, 32};

/**
 * The same list without the character controller, for keeping bodies off **weapons**.
 *
 * **The weapon against the character controller is how the game lands a hit**, measured the hard way on
 * 2026-09-12: clearing it left every player unable to touch an NPC while NPCs went on hitting them, which is the
 * asymmetry the mirrors produce, since a mirrored capsule keeps the weapon bit the vanilla one lost. So that one
 * pair stays, and a body walking its capsule over a dropped sword can still nudge it.
 *
 * The limbs go, and by the same evidence that settled clutter: 33 is what a warped body ejects things with, and
 * clearing 30 against clutter on its own changed nothing while clearing 33 fixed it.
 */
constexpr uint32_t kWeaponSafeBodyLayers[] = {33, 8, 32};

// Rows 55 and up, which is where the names the filter keeps beside the table run out: measured 2026-09-12, VR
// names 0 through 54 and leaves nine slots of a fixed array unnamed and set to every bit. Candidates, not
// choices, because a mod can claim one, so every row is still tested for being unused before it is written.
constexpr uint32_t kLowestSpareLayer = 55;

/**
 * @brief The layer each of kActorLayers is mirrored onto for the local player, zero until one has been chosen.
 */
uint32_t s_mirrorLayers[kActorLayerCount]{};
bool s_mirrorsChosen = false;

// The collision layer a havok world object is on, or UINT32_MAX if it cannot be read. Defined below.
uint32_t BodyLayer(const uint8_t* acpWorldObject) noexcept;

// Whether a layer is one a character's own collision sits on, counting the mirrors as the layers they stand in
// for. Used to confirm that something found by searching really is an actor's body.
bool IsActorLayer(const uint32_t aLayer) noexcept
{
    for (size_t i = 0; i < kActorLayerCount; ++i)
        if (aLayer == kActorLayers[i] || (s_mirrorLayers[i] && aLayer == s_mirrorLayers[i]))
            return true;

    return false;
}

const uint8_t* ReadPointer(const uint8_t* acpAt) noexcept
{
    return IsReadable(acpAt, sizeof(void*)) ? *reinterpret_cast<const uint8_t* const*>(acpAt) : nullptr;
}

/**
 * @brief Whether a candidate is an instance of the wanted class, remembering the answer for each vtable.
 *
 * `RttiName` and `RttiNameContains` cost a VirtualQuery per read, and the walks below meet the same handful of
 * classes hundreds of times a pass. A vtable identifies a class exactly, so each one is named once and its
 * answer kept. Keyed on the wanted text as well, which is a literal at every call site.
 */
bool IsInstanceOf(const void* acpCandidate, const char* acpWanted) noexcept
{
    static Map<const char*, Map<const void*, bool>> s_named;

    if (!IsReadable(acpCandidate, sizeof(void*)))
        return false;

    const auto* cpVtable = *reinterpret_cast<const void* const*>(acpCandidate);

    auto& cache = s_named[acpWanted];

    if (const auto cIt = cache.find(cpVtable); cIt != cache.end())
        return cIt->second;

    const bool cMatches = RttiNameContains(RttiName(acpCandidate), acpWanted);

    cache[cpVtable] = cMatches;

    return cMatches;
}

/**
 * @brief The member of acpBase whose RTTI names the wanted class, looked for at aPinned first.
 *
 * The pinned offsets were measured by this same search on 2026-08-30 and written down, which keeps the common
 * path at one read and one cached vtable lookup. Nothing is assumed: a member of the wrong type falls back to
 * the search, and the search says where the member went, the way each offset was arrived at in the first place.
 * A member that is simply absent reads as null and is not searched for, because that is an object still being
 * built rather than a field that moved.
 */
const uint8_t* FindMemberByRtti(const uint8_t* acpBase, const size_t aPinned, const size_t aSearchEnd, const char* acpWanted) noexcept
{
    const uint8_t* cpPinned = ReadPointer(acpBase + aPinned);

    if (!cpPinned)
        return nullptr;

    if (IsInstanceOf(cpPinned, acpWanted))
        return cpPinned;

    for (size_t offset = 0; offset + sizeof(void*) <= aSearchEnd; offset += sizeof(void*))
    {
        const uint8_t* cpCandidate = ReadPointer(acpBase + offset);

        if (!cpCandidate || !IsInstanceOf(cpCandidate, acpWanted))
            continue;

        spdlog::warn("[clutter] '{}' is at +{:#05X}, not the +{:#05X} this was pinned to", acpWanted, offset, aPinned);

        return cpCandidate;
    }

    return nullptr;
}

/**
 * @brief The havok body of an actor's character controller.
 *
 * Measured on SkyrimVR 1.4.15 on 2026-08-23: MiddleProcess+0x250 holds a bhkCharRigidBodyController, its +0x360
 * a bhkRigidBody, and a bhkRefObject keeps the havok object it wraps at +0x10. The concrete class is never
 * `bhkCharacterController`, which is the abstract base and does not appear by name.
 */
/**
 * @brief What kind of object owns a havok world object: 1 an entity, which is every rigid body, 2 a phantom,
 *        which is what a character proxy collides through. 0xFF if it cannot be read.
 *
 * The collidable's broad phase handle sits just before the filter word, and havok puts its type byte four bytes
 * ahead of that word. Logged beside every candidate rather than trusted, because it is arithmetic off a
 * measured offset rather than a measurement of its own, and it only ever narrows a search.
 */
uint8_t BodyBroadPhaseType(const uint8_t* acpWorldObject) noexcept
{
    constexpr size_t kBroadPhaseTypeOffset = 0x48;

    return IsReadable(acpWorldObject + kBroadPhaseTypeOffset, 1) ? *(acpWorldObject + kBroadPhaseTypeOffset) : 0xFF;
}

/**
 * @brief The havok world object under acpBase on the wanted layer, or on any layer a character uses when
 *        aWantedLayer is 0.
 *
 * An hkpWorldObject keeps the physics world it belongs to at +0x10, and a world says what it is, while +0x4C is
 * its collision filter word. A pointer that satisfies both is a body in the player's world and a stray one is
 * not. Phantoms only, when asked, because a character proxy keeps the bodies it is currently touching in its own
 * arrays: the floor, the clutter it is standing in, and the NPC it is leaning on, whose controller body is on
 * exactly the layer being looked for. Those are all entities, so the type byte separates them from the one
 * phantom that belongs to the proxy itself.
 */
const uint8_t* FindBodyUnder(const uint8_t* acpBase, const size_t aSearchEnd, const uint32_t aWantedLayer, const bool aPhantomsOnly) noexcept
{
    constexpr size_t kWorldOffset = 0x10;
    constexpr uint8_t kPhantom = 2;

    for (size_t offset = 0; offset + sizeof(void*) <= aSearchEnd; offset += sizeof(void*))
    {
        const uint8_t* cpCandidate = ReadPointer(acpBase + offset);

        if (!cpCandidate || !IsInstanceOf(ReadPointer(cpCandidate + kWorldOffset), "World"))
            continue;

        if (aPhantomsOnly && BodyBroadPhaseType(cpCandidate) != kPhantom)
            continue;

        const uint32_t cLayer = BodyLayer(cpCandidate);

        if (aWantedLayer ? cLayer == aWantedLayer : IsActorLayer(cLayer))
            return cpCandidate;
    }

    return nullptr;
}

const uint8_t* FindControllerBody(Actor* apActor) noexcept
{
    constexpr size_t kCharControllerOffset = 0x250;
    constexpr size_t kRigidBodyOffset = 0x360;
    constexpr size_t kReferencedObjectOffset = 0x10;
    constexpr uint32_t kCharControllerLayer = 30;

    if (!apActor || !apActor->currentProcess || !apActor->currentProcess->middleProcess)
        return nullptr;

    const auto* cpProcess = reinterpret_cast<const uint8_t*>(apActor->currentProcess->middleProcess);

    // "bhkChar" rather than a concrete class, because the two kinds of actor do not share one. Measured
    // 2026-09-12: this slot is a bhkCharRigidBodyController on every NPC and a bhkCharProxyController on the
    // local player, and asking for the rigid body name is how the player's collision went unfound all session.
    const uint8_t* cpController = FindMemberByRtti(cpProcess, kCharControllerOffset, 0x400, "bhkChar");
    if (!cpController)
        return nullptr;

    // The rigid body case, which is every NPC. A pinned read and nothing more: searching here would find some
    // other rigid body inside a proxy controller and mirror the wrong one, and anything the pin misses the
    // fingerprint below finds anyway.
    const uint8_t* cpBhkBody = ReadPointer(cpController + kRigidBodyOffset);

    if (cpBhkBody && IsInstanceOf(cpBhkBody, "bhkRigidBody"))
    {
        if (const uint8_t* cpWorldObject = ReadPointer(cpBhkBody + kReferencedObjectOffset); IsActorLayer(BodyLayer(cpWorldObject)))
            return cpWorldObject;
    }

    /**
     * The proxy case, which is the local player, found by fingerprint because nothing about it can be reached by
     * a known offset: a proxy collides through a phantom, the chain to it runs through havok classes this client
     * has not mapped, and havok's own classes cannot be relied on to carry RTTI.
     *
     * **The layer is part of the search, not something to be read off the first hit.** The first version took
     * whatever world object came first and got the one at controller+0x2B0, which is on layer 5, so the body
     * that was mirrored was not the one that walks into furniture and the player's own capsule stayed on 30
     * where the table no longer lets it touch clutter. Measured 2026-09-12: a player who collides with NPCs and
     * with weapons but not with items is a player whose capsule was never moved.
     */
    if (const uint8_t* cpBody = FindBodyUnder(cpController, 0x400, kCharControllerLayer, false))
        return cpBody;

    /**
     * One step further in, for a proxy that keeps its phantom on the havok object rather than on the wrapper.
     * Phantoms only, so the bodies a proxy is merely touching cannot be mistaken for its own.
     *
     * Capped, because this is thousands of guarded reads and a resolve that keeps failing would run it again
     * every time the pass wakes up. Three sessions' worth of evidence that searching does not find it is enough
     * to stop searching and say so.
     */
    static uint32_t s_deepFailures = 0;

    if (s_deepFailures < 3)
    {
        for (size_t offset = 0; offset + sizeof(void*) <= 0x400; offset += sizeof(void*))
        {
            const uint8_t* cpInner = ReadPointer(cpController + offset);

            if (!cpInner)
                continue;

            if (const uint8_t* cpBody = FindBodyUnder(cpInner, 0x200, kCharControllerLayer, true))
            {
                spdlog::info("[clutter] the local player's own phantom is a step in, under controller+{:#05X}", offset);

                return cpBody;
            }
        }

        // Only a search that found nothing counts, so a cell change that has to resolve again still gets one.
        ++s_deepFailures;
    }

    // Nothing on the controller layer. Whatever character-layer body the controller does hold is better than
    // none, and the warning says which it settled for.
    const uint8_t* cpFallback = FindBodyUnder(cpController, 0x400, 0, false);

    static bool s_warnedFallback = false;

    if (cpFallback && !s_warnedFallback)
    {
        s_warnedFallback = true;
        spdlog::warn("[clutter] no body on layer {} under the local player's controller, falling back to one on layer {}, so walking into things may still do nothing", kCharControllerLayer, BodyLayer(cpFallback));
    }

    return cpFallback;
}

/**
 * @brief The collision filter of the physics world a havok body belongs to. Every cell has its own.
 */
const uint8_t* FindCollisionFilter(const uint8_t* acpHavokBody) noexcept
{
    constexpr size_t kWorldOffset = 0x10;
    constexpr size_t kFilterOffset = 0xD0;

    if (!acpHavokBody)
        return nullptr;

    const uint8_t* cpWorld = FindMemberByRtti(acpHavokBody, kWorldOffset, 0x400, "World");
    if (!cpWorld)
        return nullptr;

    return FindMemberByRtti(cpWorld, kFilterOffset, 0x600, "CollisionFilter");
}

/**
 * @brief Whether the 64 entries here read like the layer-versus-layer table.
 *
 * The table is the one step of the walk with no type information, so it is recognised by a property only a real
 * one has: symmetry. If layer i meets layer j then j meets i, so bit j of entry i equals bit i of entry j across
 * the whole table. Padding, pointer runs and vtables do not satisfy that by accident. Measured on 2026-08-30 at
 * filter+0x1D0, where 1421 of 1431 layer pairs agreed, against a vtable at offset 0 that an earlier and looser
 * test accepted while its own numbers contradicted each other. Read as 64 bit masks, because with up to 64
 * layers a mask has to be that wide.
 */
bool ReadsLikeLayerTable(const uint64_t* acpTable) noexcept
{
    if (!IsReadable(acpTable, kLayerCount * sizeof(uint64_t)))
        return false;

    // The character controller's row, which in a real table is neither empty nor everything.
    constexpr uint32_t kCharControllerLayer = 30;

    if (acpTable[kCharControllerLayer] == 0 || acpTable[kCharControllerLayer] == ~0ull || acpTable[kClutterLayer] == 0)
        return false;

    size_t checked = 0;
    size_t symmetric = 0;

    for (size_t i = 0; i < kLayerCount; ++i)
    {
        if (acpTable[i] == 0 || acpTable[i] == ~0ull)
            continue;

        for (size_t j = i + 1; j < kLayerCount; ++j)
        {
            if (acpTable[j] == 0 || acpTable[j] == ~0ull)
                continue;

            ++checked;
            symmetric += ((acpTable[i] >> j) & 1) == ((acpTable[j] >> i) & 1);
        }
    }

    return checked >= 64 && symmetric * 10 >= checked * 9;
}

uint64_t* FindLayerTable(const uint8_t* acpFilter) noexcept
{
    constexpr size_t kPinnedOffset = 0x1D0;
    constexpr size_t kSearchEnd = 0x800;

    auto* pPinned = const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(acpFilter + kPinnedOffset));

    if (ReadsLikeLayerTable(pPinned))
        return pPinned;

    for (size_t offset = 0; offset + kLayerCount * sizeof(uint64_t) <= kSearchEnd; offset += sizeof(uint64_t))
    {
        auto* pCandidate = const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(acpFilter + offset));

        if (!ReadsLikeLayerTable(pCandidate))
            continue;

        spdlog::warn("[clutter] the layer table is at filter+{:#05X}, not the +{:#05X} this was pinned to", offset, kPinnedOffset);

        return pCandidate;
    }

    return nullptr;
}

/**
 * @brief Whether nothing in the table uses a layer, so its row can be rewritten.
 *
 * Two kinds of row count as unused, which is the correction that made this work at all. Measured on SkyrimVR
 * 1.4.15 on 2026-09-12 from a full dump of the table: rows 0 to 54 are real masks, VR having rather more layers
 * than the 47 Skyrim SE names, row 49 is empty, and rows 55 to 63 are every bit set. Nine identical all-ones
 * rows at the end of the array are the tail of a fixed 64 entry array nobody filled in, not layers that meet
 * everything, and the table proves it: a row claiming to meet clutter would have its bit set in the clutter row
 * if it were real, and none of 55 to 63 is referenced by any of the 55 real rows.
 *
 * The first version demanded an empty row and so rejected all nine, which is why a session reported no free row
 * and the local player got nothing back.
 */
bool IsLayerUnused(const uint64_t* acpTable, const uint32_t aLayer) noexcept
{
    if (acpTable[aLayer] != 0 && acpTable[aLayer] != ~0ull)
        return false;

    for (size_t i = 0; i < kLayerCount; ++i)
        if (acpTable[i] != ~0ull && ((acpTable[i] >> aLayer) & 1))
            return false;

    return true;
}

// Highest row first, so the ones a mod is most likely to have claimed are met before the rows next to the
// layers Skyrim itself uses. A row already in use is skipped rather than fought over.
void ChooseMirrorLayers(const uint64_t* acpTable) noexcept
{
    uint32_t candidate = kLayerCount - 1;

    // As many as there are rows for, in the order kActorLayers puts them. Fewer than four is a real outcome
    // rather than an error: the rows that do get one work, and a layer left without keeps the game's own
    // behaviour, which is no clutter collision for anybody.
    for (size_t i = 0; i < kActorLayerCount; ++i)
    {
        while (candidate >= kLowestSpareLayer && !IsLayerUnused(acpTable, candidate))
            --candidate;

        if (candidate < kLowestSpareLayer)
            break;

        s_mirrorLayers[i] = candidate--;
    }

    if (!s_mirrorLayers[0])
    {
        spdlog::warn("[clutter] the layer table has no free row at all between {} and {}, so nobody pushes clutter about, the local player included", kLowestSpareLayer, kLayerCount - 1);

        return;
    }

    for (size_t i = 0; i < kActorLayerCount; ++i)
        spdlog::info("[clutter] the local player's layer {} is mirrored onto row {}, where 0 means no row was free for it", kActorLayers[i], s_mirrorLayers[i]);
}

/**
 * @brief Gives the local player's layers a mirror of their own, then takes clutter away from the originals.
 *
 * A mirror row is a copy: a mirrored body meets everything the layer it came from met, clutter included, and
 * both directions are written because havok consults whichever body it tests first. So the player's sword still
 * hits actors and their controller still meets other controllers and the world, and the only difference between
 * a mirror and its original is the one pair this then clears on the original.
 *
 * The price of the clear is unchanged and is paid by everyone else: an NPC no longer knocks a cup off a table,
 * and neither does another player's body. Hits on actors are unaffected either way, because those are the
 * weapon against the biped and controller layers and only the pair with clutter is touched.
 */
void PatchLayerTable(uint64_t* apTable) noexcept
{
    uint64_t vanilla[kActorLayerCount]{};

    for (size_t i = 0; i < kActorLayerCount; ++i)
        vanilla[i] = apTable[kActorLayers[i]];

    if (!s_mirrorsChosen)
    {
        s_mirrorsChosen = true;
        ChooseMirrorLayers(apTable);
    }

    uint64_t mirrored[kActorLayerCount]{};

    for (size_t i = 0; i < kActorLayerCount && s_mirrorLayers[i]; ++i)
    {
        mirrored[i] = vanilla[i];

        // The player's own mirrored bodies meet each other the way the layers they came from did.
        for (size_t j = 0; j < kActorLayerCount; ++j)
            if ((vanilla[i] >> kActorLayers[j]) & 1)
                mirrored[i] |= 1ull << s_mirrorLayers[j];
    }

    for (size_t i = 0; i < kActorLayerCount && s_mirrorLayers[i]; ++i)
    {
        apTable[s_mirrorLayers[i]] = mirrored[i];

        for (uint32_t layer = 0; layer < kLayerCount; ++layer)
            if ((mirrored[i] >> layer) & 1)
                apTable[layer] |= 1ull << s_mirrorLayers[i];
    }

    /**
     * The pairs themselves. Both directions every time, because havok consults whichever of the two bodies it
     * tests first and a table that disagrees with itself is worse than one that does not.
     *
     * Clutter takes the whole body list. Weapons take it without the character controller, which is the pair the
     * game lands hits through and the one pair here that cannot be closed. And the held weapon is kept off
     * clutter on its own, since the body list deliberately leaves the weapon layer out.
     */
    const auto cClearPair = [apTable](const uint32_t aFirst, const uint32_t aSecond) noexcept
    {
        apTable[aFirst] &= ~(1ull << aSecond);
        apTable[aSecond] &= ~(1ull << aFirst);
    };

    for (const uint32_t cBodyLayer : kActorBodyLayers)
        cClearPair(cBodyLayer, kClutterLayer);

    for (const uint32_t cBodyLayer : kWeaponSafeBodyLayers)
        cClearPair(cBodyLayer, kWeaponLayer);

    cClearPair(kWeaponLayer, kClutterLayer);

    if (s_mirrorLayers[0])
        spdlog::info(
            "[clutter] characters no longer shove clutter or weapons about. The local player's controller mirror, layer {}, has mask {:#018X}, and still meets clutter {}, weapons {}, controllers {} and limbs with no controller {}",
            s_mirrorLayers[0], apTable[s_mirrorLayers[0]], (apTable[s_mirrorLayers[0]] >> kClutterLayer) & 1, (apTable[s_mirrorLayers[0]] >> kWeaponLayer) & 1, (apTable[s_mirrorLayers[0]] >> 30) & 1,
            (apTable[s_mirrorLayers[0]] >> 33) & 1);
    else
        spdlog::info("[clutter] characters no longer shove clutter or weapons about, the local player included. Clutter now meets {:#018X} and weapons {:#018X}", apTable[kClutterLayer], apTable[kWeaponLayer]);
}

/**
 * @brief The patched table of the world a filter belongs to, patching it the first time that world is seen.
 *
 * Every cell has its own physics world and its own filter, so this is tracked per filter rather than run once.
 * A filter that could not be patched is remembered too, so its failure is reported once rather than every tick.
 */
uint64_t* GetPatchedLayerTable(const uint8_t* acpFilter) noexcept
{
    static Map<const void*, uint64_t*> s_tables;

    if (!acpFilter)
        return nullptr;

    if (const auto cIt = s_tables.find(acpFilter); cIt != s_tables.end())
        return cIt->second;

    uint64_t* pTable = FindLayerTable(acpFilter);

    s_tables[acpFilter] = pTable;

    if (!pTable)
    {
        spdlog::warn("[clutter] nothing in this world's collision filter reads like a symmetric 64 layer table, so characters keep their clutter collision and items will drift");

        return nullptr;
    }

    PatchLayerTable(pTable);

    return pTable;
}

/**
 * @brief Moves one body between its layer and that layer's mirror, leaving the rest of the filter word alone.
 *
 * Only the seven layer bits are touched. The rest of the word is a collision group and whatever else the filter
 * encodes, and none of it is ours to reinterpret. A body the game has put somewhere else of its own accord,
 * L_NONCOLLIDABLE for a player in furniture or on a cart, matches nothing here and is left exactly as it is.
 */
bool MoveBodyToMirror(const uint8_t* acpHavokBody, const bool aToMirror) noexcept
{
    constexpr size_t kCollisionFilterOffset = 0x4C;
    constexpr uint32_t kLayerMask = 0x7F;

    if (!acpHavokBody || !IsReadable(acpHavokBody + kCollisionFilterOffset, sizeof(uint32_t)))
        return false;

    auto* pFilterWord = const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(acpHavokBody + kCollisionFilterOffset));

    const uint32_t cLayer = *pFilterWord & kLayerMask;

    for (size_t i = 0; i < kActorLayerCount; ++i)
    {
        if (!s_mirrorLayers[i] || cLayer != (aToMirror ? kActorLayers[i] : s_mirrorLayers[i]))
            continue;

        *pFilterWord = (*pFilterWord & ~kLayerMask) | (aToMirror ? s_mirrorLayers[i] : kActorLayers[i]);

        return true;
    }

    return false;
}

/**
 * @brief Everything the local player's own collision goes through, once it has been found.
 *
 * Kept between passes because the search behind it is thousands of guarded reads and has to stay off the frame
 * path, and kept at this scope because leaving a session has to put every one of them back.
 */
Vector<const uint8_t*> s_playerBodies{};

// The root these bodies were resolved against. A rebuilt 3D means a rebuilt controller, and the old pointers
// are then somebody else's memory.
const void* s_playerBodiesRoot = nullptr;

// Whether the player is currently carrying mirror layers, so that putting them back can be skipped on every
// frame it is not needed and run on the one frame it is.
bool s_playerMirrored = false;

uint32_t BodyLayer(const uint8_t* acpWorldObject) noexcept
{
    constexpr size_t kCollisionFilterOffset = 0x4C;
    constexpr uint32_t kLayerMask = 0x7F;

    if (!acpWorldObject || !IsReadable(acpWorldObject + kCollisionFilterOffset, sizeof(uint32_t)))
        return UINT32_MAX;

    return *reinterpret_cast<const uint32_t*>(acpWorldObject + kCollisionFilterOffset) & kLayerMask;
}

/**
 * @brief Whether the kept bodies are still the player's own.
 *
 * Three reads each, which is what makes it safe to check every pass rather than searching again. A body still
 * pointing at a world that says it is one, and still on a layer a character uses, is the body that was found.
 * Both halves are needed: a freed block can read as a plausible layer by chance, and writing a layer into
 * whatever now owns that memory is exactly the mistake the hand pose cache made in August.
 */
bool PlayerBodiesStillGood() noexcept
{
    constexpr size_t kWorldOffset = 0x10;

    if (s_playerBodies.empty())
        return false;

    for (const uint8_t* cpBody : s_playerBodies)
    {
        if (!IsActorLayer(BodyLayer(cpBody)) || !IsInstanceOf(ReadPointer(cpBody + kWorldOffset), "World"))
            return false;
    }

    return true;
}

/**
 * @brief Finds the bodies the local player's controller holds: the capsule they walk around in, and whatever
 *        else of their own hangs off it.
 *
 * More than one, deliberately. The controller holds world objects besides its own phantom, and the player is
 * meant to collide with everything, so every body of theirs on a character layer is taken rather than only the
 * capsule. The capsule goes first, since that is the one that matters.
 */
void ResolvePlayerBodies(Actor* apPlayer) noexcept
{
    constexpr size_t kCharControllerOffset = 0x250;
    constexpr size_t kWorldOffset = 0x10;

    s_playerBodies.clear();

    if (const uint8_t* cpPrimary = FindControllerBody(apPlayer))
        s_playerBodies.push_back(cpPrimary);

    if (!apPlayer->currentProcess || !apPlayer->currentProcess->middleProcess)
        return;

    const auto* cpProcess = reinterpret_cast<const uint8_t*>(apPlayer->currentProcess->middleProcess);

    const uint8_t* cpController = FindMemberByRtti(cpProcess, kCharControllerOffset, 0x400, "bhkChar");
    if (!cpController)
        return;

    for (size_t offset = 0; offset + sizeof(void*) <= 0x400; offset += sizeof(void*))
    {
        const uint8_t* cpCandidate = ReadPointer(cpController + offset);

        if (!cpCandidate || !IsInstanceOf(ReadPointer(cpCandidate + kWorldOffset), "World") || !IsActorLayer(BodyLayer(cpCandidate)))
            continue;

        bool known = false;

        for (const uint8_t* cpBody : s_playerBodies)
            known = known || cpBody == cpCandidate;

        if (!known)
            s_playerBodies.push_back(cpCandidate);
    }
}

/**
 * @brief Every havok body hanging off a node tree.
 *
 * A node keeps its collision object among the pointers between its parent and its local transform, and the
 * collision object keeps its body in its own first few. Both are found by type rather than at a fixed offset:
 * NiAVObject is unmapped in this client past the transforms measured in ObjectService, so the search is three
 * candidates wide, and a node with no collision simply has nothing of that type to find. The offset the first
 * body turns up at is kept and tried on its own from then on, because `IsReadable` is a VirtualQuery and this
 * walk is the one place in the client that does hundreds of them at a time.
 *
 * A living player's 3D holds none of these: measured on 2026-09-12 across all 110 nodes of skeleton.nif, the
 * equipped weapon's own node included. It is walked anyway, because a body that does appear there, a drawn
 * weapon or whatever a hand physics mod adds, is the player's and belongs on the mirror layers with the rest.
 */
void CollectTreeBodies(NiAVObject* apNode, const uint32_t aDepth, Vector<const uint8_t*>& aBodies) noexcept
{
    constexpr size_t kCollisionSearchStart = 0x30; // parent, the first member past NiObjectNET
    constexpr size_t kCollisionSearchEnd = 0x48;   // the local transform, measured in ObjectService
    constexpr size_t kBodySearchEnd = 0x40;
    constexpr size_t kReferencedObjectOffset = 0x10;
    constexpr uint32_t kMaxDepth = 32;
    constexpr uint16_t kMaxChildren = 512;
    constexpr size_t kMaxBodies = 256;

    static size_t s_collisionOffset = 0;

    if (!IsReadable(apNode, kNiAVObjectSize) || aDepth > kMaxDepth || aBodies.size() >= kMaxBodies)
        return;

    const auto* cpNode = reinterpret_cast<const uint8_t*>(apNode);

    for (size_t offset = s_collisionOffset ? s_collisionOffset : kCollisionSearchStart; offset < kCollisionSearchEnd; offset += sizeof(void*))
    {
        const uint8_t* cpCollision = ReadPointer(cpNode + offset);

        if (!cpCollision || !IsInstanceOf(cpCollision, "CollisionObject"))
        {
            if (s_collisionOffset)
                break;

            continue;
        }

        for (size_t bodyOffset = 0; bodyOffset < kBodySearchEnd; bodyOffset += sizeof(void*))
        {
            const uint8_t* cpBhkBody = ReadPointer(cpCollision + bodyOffset);

            if (!cpBhkBody || !IsInstanceOf(cpBhkBody, "bhkRigidBody"))
                continue;

            if (const uint8_t* cpWorldObject = ReadPointer(cpBhkBody + kReferencedObjectOffset))
            {
                aBodies.push_back(cpWorldObject);

                s_collisionOffset = offset;
            }

            break;
        }

        break;
    }

    if (!IsReadable(apNode, sizeof(NiNode)))
        return;

    auto* pAsNode = static_cast<NiNode*>(apNode);

    // Whether this is an NiNode at all is not knowable from here, so the children array is sanity checked
    // before it is believed: geometry read as a node gives a length that fails this.
    if (pAsNode->children.length > kMaxChildren || !pAsNode->children.data || !IsReadable(pAsNode->children.data, sizeof(void*) * pAsNode->children.length))
        return;

    for (uint16_t i = 0; i < pAsNode->children.length; ++i)
        CollectTreeBodies(pAsNode->children[i], aDepth + 1, aBodies);
}

/**
 * @brief Puts the local player's own bodies on the mirror layers, so the one character that is never teleported
 *        keeps colliding with everything while nobody else shoves it about.
 *
 * Re-applied rather than done once, because the game writes a body's layer itself on a 3D rebuild, on a weapon
 * being drawn and on death, and each of those hands the body back on the layer the table no longer lets touch
 * anything loose.
 *
 * **Never per frame.** The first version resolved the player's controller on every frame, and when that resolve
 * failed, which it did all session because the player has no rigid body controller, each frame ran a bounded
 * search instead: a thousand VirtualQuery calls at 72Hz, which is the lag reported on 2026-09-12. What decides
 * when this runs is in CollisionService::OnUpdate, and it is made of reads that cost nothing.
 */
bool MirrorLocalPlayerCollision() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;

    NiNode* pRoot = pPlayer->GetNiNode();

    if (pRoot != s_playerBodiesRoot || !PlayerBodiesStillGood())
    {
        s_playerBodiesRoot = pRoot;
        ResolvePlayerBodies(pPlayer);
    }

    Vector<const uint8_t*> bodies{};

    CollectTreeBodies(pRoot, 0, bodies);

    // The controller's bodies first, because on the local player they are the only ones there are.
    bodies.insert(bodies.begin(), s_playerBodies.begin(), s_playerBodies.end());

    static bool s_warnedEmpty = false;

    if (bodies.empty())
    {
        if (!s_warnedEmpty)
        {
            s_warnedEmpty = true;
            spdlog::warn("[clutter] the local player has no collision this can find, neither a controller body nor anything in their 3D, so they keep no collision with objects at all");
        }

        return false;
    }

    // Any of them will do: they are all in the world the player is standing in. A body that has been taken out
    // of its world has no world pointer, so this tries the next one rather than giving up.
    uint64_t* pTable = nullptr;

    for (const uint8_t* cpBody : bodies)
    {
        pTable = GetPatchedLayerTable(FindCollisionFilter(cpBody));

        if (pTable)
            break;
    }

    if (!pTable)
        return false;

    // The table is patched either way, which is what the caller wants to know. Whether the player is exempt from
    // it depends on there having been a spare row to put them on.
    if (!s_mirrorLayers[0])
        return true;

    uint32_t moved = 0;

    for (const uint8_t* cpBody : bodies)
        moved += MoveBodyToMirror(cpBody, true);

    s_playerMirrored = true;

    static size_t s_lastFound = 0;
    static uint32_t s_lastMoved = UINT32_MAX;

    if (bodies.size() == s_lastFound && moved == s_lastMoved)
        return true;

    s_lastFound = bodies.size();
    s_lastMoved = moved;

    spdlog::info("[clutter] the local player has {} collision bodies, {} of them moved onto the mirror layers, the first now on layer {}", bodies.size(), moved, BodyLayer(bodies[0]));

    return true;
}

/**
 * @brief Puts the local player's bodies back on the layers the game gave them.
 *
 * For leaving a session, and for the setting being turned off under a session that is already running. A mirror
 * row only means anything in the table of a world that was patched, and an unpatched one reads as every bit set,
 * measured 2026-09-12. So a body left on a mirror layer in a world nobody patched collides with everything: pick
 * layers, line of sight, trigger volumes, camera spheres. Asymmetrically, too, since no other row mentions that
 * bit, which leaves havok's answer depending on which of the two bodies it happens to test first.
 */
void ReturnLocalPlayerCollision() noexcept
{
    s_playerMirrored = false;

    if (!s_mirrorLayers[0])
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    Vector<const uint8_t*> bodies{};

    CollectTreeBodies(pPlayer->GetNiNode(), 0, bodies);

    bodies.insert(bodies.begin(), s_playerBodies.begin(), s_playerBodies.end());

    uint32_t moved = 0;

    for (const uint8_t* cpBody : bodies)
        moved += MoveBodyToMirror(cpBody, false);

    spdlog::info("[clutter] the local player's collision is back on its own layers, {} bodies moved", moved);
}

/**
 * @brief Stops characters pushing clutter about in whichever physics world this actor is standing in.
 *
 * Reached when a remote body is being set up, which can be a world the local player is not in yet.
 */
void DisableCharacterClutterCollision(Actor* apActor) noexcept
{
    GetPatchedLayerTable(FindCollisionFilter(FindControllerBody(apActor)));
}

/**
 * @brief Patches the table of whichever physics world a remote body has been set up in.
 *
 * The fallback for a build where the player's own collision cannot be found. The pass below leads with the
 * player's bodies, because their world is the one that matters and their bodies are what the mirrors are for, but
 * if that resolve ever fails the clear itself must still happen: everyone shoving furniture is a worse outcome
 * than the player not being exempt from it. Any remote body in a loaded cell leads to the same table.
 */
void PatchWorldOfAnyRemoteBody(World& aWorld) noexcept
{
    for (auto entity : aWorld.view<FormIdComponent, RemoteComponent>())
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aWorld.get<FormIdComponent>(entity).Id));

        if (pActor && GetPatchedLayerTable(FindCollisionFilter(FindControllerBody(pActor))))
            return;
    }
}
} // namespace
#endif

CollisionService::CollisionService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CollisionService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CollisionService::OnDisconnected>(this);
}

void CollisionService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
#if TP_SKYRIMVR
    // Long, because the backstop is for something none of the tests below can see coming. Everything that does
    // change which bodies the player has, or which world they are in, is watched for directly.
    constexpr double kBackstopInterval = 5.0;

    /**
     * Off is the game exactly as it shipped: nothing patched, no layer moved, and item drift is the game's own
     * behaviour. Turning it off under a running session puts the player's own layers back but leaves the table
     * patched, since the rows it replaced are not kept anywhere. A session that starts with it off never patches
     * at all, so there is nothing to undo.
     */
    if (!m_transport.IsOnline() || !m_world.GetServerSettings().DisableCollisionBetweenOtherCharactersAndObjects)
    {
        // A bool, so the frame this is false on costs one load. The restore walks and must run on the frame the
        // answer changed rather than on every frame after it.
        if (s_playerMirrored)
            ReturnLocalPlayerCollision();

        return;
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const void* cpRoot = pPlayer->GetNiNode();
    const void* cpCell = pPlayer->parentCell;
    const bool cDrawn = pPlayer->actorState.IsWeaponDrawn();

    m_sinceLastPass += acEvent.Delta;

    // The cell is in here because every cell has its own physics world and its own table, and a body carrying a
    // mirror layer into a world nobody has patched is on a row that reads as every bit set. One frame of that on
    // a door transition is the whole reason this is not left to the backstop.
    if (cpRoot == m_root && cpCell == m_cell && cDrawn == m_weaponDrawn && m_sinceLastPass < kBackstopInterval)
        return;

    m_root = cpRoot;
    m_cell = cpCell;
    m_weaponDrawn = cDrawn;
    m_sinceLastPass = 0.0;

    if (!MirrorLocalPlayerCollision())
        PatchWorldOfAnyRemoteBody(m_world);
#endif
}

void CollisionService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
#if TP_SKYRIMVR
    ReturnLocalPlayerCollision();

    m_root = nullptr;
    m_cell = nullptr;
    m_sinceLastPass = 0.0;
#endif
}
