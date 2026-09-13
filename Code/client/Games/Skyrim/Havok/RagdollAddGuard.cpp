#include <TESObjectREFR.h>

#if TP_SKYRIMVR

#include <Actor.h>
#include <Forms/TESNPC.h>
#include <Forms/TESRace.h>
#include <Games/ActorExtension.h>
#include <NetImmerse/NiAVObject.h>

#include <mutex>

/**
 * @brief Refuses to put a ragdoll into the physics world when its body or constraint list holds a pointer that
 * cannot be what it claims to be, and names the actor it belongs to.
 *
 * The crash is `SkyrimVR.exe+0AB1ABA`, seen once, on 2026-09-13. CrashLogger names it `hkpWorld::RemoveEntity`,
 * which is only the nearest id below the address. Unwound from the unpacked image, the real chain is
 * `Actor::KillImpl` -> `BShkbAnimationGraph::AddRagdollToWorld` -> this function -> `hkpWorld::addConstraint`,
 * which faulted reading `m_data` out of constraint 1: the pointer was `0x200000000`. Constraint 0 had gone in
 * cleanly. The actor was a rabbit, `105A35`, killed by a wolf on the machine that had just claimed it.
 *
 * What left the list in that state is not known, and the crash was never caught a second time, so this exists to
 * catch the next one with a name attached rather than to fix it. A leveled creature swapping base on a 3D
 * reload was the first suspect and is a weak one: the same reference rolls fox or rabbit on almost every save
 * load, offline too, and nothing crashes.
 *
 * Skipping the add is safe for the matching removal. `hkaRagdollInstance::removeFromWorld` at VR `0x140B50D00`
 * returns at once when the first rigid body has no world, so a ragdoll that was never added is never walked on
 * the way out. The body does without a ragdoll, which beats the game going down. What that looks like has not
 * been seen yet.
 *
 * The cost is one `VirtualQuery` per body and two per constraint, paid only when a ragdoll is added: a death, a
 * knockdown, a stagger into ragdoll. Never per frame.
 */

namespace
{
// `hkaRagdollInstance`, as `addToWorld` reads it: rigid bodies then constraints, each an `hkArray` of pointers.
constexpr size_t kBodiesOffset = 0x10;
constexpr size_t kBodyCountOffset = 0x18;
constexpr size_t kConstraintsOffset = 0x20;
constexpr size_t kConstraintCountOffset = 0x28;

// `hkpConstraintInstance::m_data`, the first thing `hkpWorld::addConstraint` dereferences (`mov rcx, [rdx+0x18]`
// at VR `0x140AB1ABA`, the fault). The furthest field it reads off the instance is +0x80.
constexpr size_t kConstraintDataOffset = 0x18;
constexpr size_t kConstraintReadSpan = 0x88;

// No ragdoll in the game has anywhere near this many parts. A count past it is not a count.
constexpr int32_t kMaxParts = 256;

// Key 2 of an `hkpWorldObject`'s property array is its `bhkNiCollisionObject`, and +0x10 of that is the 3D it
// belongs to. The same chain ShadowSceneAttachGuard.cpp takes from the Havok side back to a reference.
constexpr uint32_t kCollisionObjectProperty = 2;
constexpr size_t kSceneObjectOffset = 0x10;

using TGetProperty = void**(void** apResult, void* apEntity, uint32_t aKey);
using TFindReferenceFor3D = TESObjectREFR*(NiAVObject* apObject3D);
using TAddToWorld = int(void* apRagdoll, void* apWorld, bool aUpdateFilter);

TAddToWorld* RealAddToWorld = nullptr;

bool IsReadable(const void* apPtr, const size_t aSize) noexcept
{
    MEMORY_BASIC_INFORMATION info{};

    if (!apPtr || !VirtualQuery(apPtr, &info, sizeof(info)))
        return false;

    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    if ((info.Protect & cReadable) == 0)
        return false;

    const auto cStart = reinterpret_cast<uintptr_t>(info.BaseAddress);

    return reinterpret_cast<uintptr_t>(apPtr) + aSize <= cStart + info.RegionSize;
}

// A vtable lives in the game image. A pointer outside it is not one, and costs nothing to rule out.
bool IsInGameImage(const void* apPtr) noexcept
{
    static const auto s_range = []() -> std::pair<uintptr_t, uintptr_t>
    {
        const auto* const cpBase = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* const cpNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(cpBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(cpBase)->e_lfanew);

        return {reinterpret_cast<uintptr_t>(cpBase), reinterpret_cast<uintptr_t>(cpBase) + cpNt->OptionalHeader.SizeOfImage};
    }();

    const auto cAddress = reinterpret_cast<uintptr_t>(apPtr);

    return cAddress >= s_range.first && cAddress < s_range.second;
}

// A pointer to a polymorphic object: readable for as far as the game will read it, with a vtable in the image.
bool IsObject(const void* apPtr, const size_t aSize) noexcept
{
    return IsReadable(apPtr, aSize) && IsInGameImage(*static_cast<void* const*>(apPtr));
}

void* PointerAt(const void* apBase, const size_t aOffset) noexcept
{
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(apBase) + aOffset);
}

int32_t CountAt(const void* apBase, const size_t aOffset) noexcept
{
    return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(apBase) + aOffset);
}

// The first part of the ragdoll that fails, described, or empty when every part passes. `aBodiesSound` says
// whether every rigid body passed, which is what looking the actor up afterwards depends on.
std::string FindBadPart(void* apRagdoll, bool& aBodiesSound) noexcept
{
    aBodiesSound = false;

    if (!IsReadable(apRagdoll, kConstraintCountOffset + sizeof(int32_t)))
        return fmt::format("the ragdoll instance {} itself is unreadable", fmt::ptr(apRagdoll));

    const int32_t cBodyCount = CountAt(apRagdoll, kBodyCountOffset);
    const int32_t cConstraintCount = CountAt(apRagdoll, kConstraintCountOffset);

    if (cBodyCount <= 0 || cBodyCount > kMaxParts || cConstraintCount < 0 || cConstraintCount > kMaxParts)
        return fmt::format("it claims {} bodies and {} constraints", cBodyCount, cConstraintCount);

    void* const pBodies = PointerAt(apRagdoll, kBodiesOffset);

    if (!IsReadable(pBodies, cBodyCount * sizeof(void*)))
        return fmt::format("its body array {} for {} bodies is unreadable", fmt::ptr(pBodies), cBodyCount);

    for (int32_t i = 0; i < cBodyCount; ++i)
    {
        void* const pBody = PointerAt(pBodies, i * sizeof(void*));

        if (!IsObject(pBody, sizeof(void*)))
            return fmt::format("body {} of {} is {}", i, cBodyCount, fmt::ptr(pBody));
    }

    aBodiesSound = true;

    if (cConstraintCount == 0)
        return {};

    void* const pConstraints = PointerAt(apRagdoll, kConstraintsOffset);

    if (!IsReadable(pConstraints, cConstraintCount * sizeof(void*)))
        return fmt::format("its constraint array {} for {} constraints is unreadable", fmt::ptr(pConstraints), cConstraintCount);

    for (int32_t i = 0; i < cConstraintCount; ++i)
    {
        void* const pConstraint = PointerAt(pConstraints, i * sizeof(void*));

        if (!IsObject(pConstraint, kConstraintReadSpan))
            return fmt::format("constraint {} of {} is {}, bodies {}", i, cConstraintCount, fmt::ptr(pConstraint), cBodyCount);

        void* const pData = PointerAt(pConstraint, kConstraintDataOffset);

        if (!IsObject(pData, sizeof(void*)))
            return fmt::format("constraint {} of {} at {} has data {}, bodies {}", i, cConstraintCount, fmt::ptr(pConstraint), fmt::ptr(pData), cBodyCount);
    }

    return {};
}

// The reference whose 3D the ragdoll's first body belongs to, or null. Only called once that body has passed.
TESObjectREFR* ResolveReference(void* apRagdoll) noexcept
{
    POINTER_SKYRIMSE(TGetProperty, s_getProperty, 77799);
    POINTER_SKYRIMSE(TFindReferenceFor3D, s_findReferenceFor3D, 19750);

    void* const pBody = PointerAt(PointerAt(apRagdoll, kBodiesOffset), 0);

    void* pCollisionObject = nullptr;
    s_getProperty.Get()(&pCollisionObject, pBody, kCollisionObjectProperty);

    if (!pCollisionObject)
        return nullptr;

    auto* const pSceneObject = static_cast<NiAVObject*>(PointerAt(pCollisionObject, kSceneObjectOffset));

    return pSceneObject ? s_findReferenceFor3D.Get()(pSceneObject) : nullptr;
}

int HookAddToWorld(void* apRagdoll, void* apWorld, bool aUpdateFilter)
{
    bool bodiesSound = false;
    const std::string cBadPart = FindBadPart(apRagdoll, bodiesSound);

    if (cBadPart.empty())
        return RealAddToWorld(apRagdoll, apWorld, aUpdateFilter);

    // Once per ragdoll instance. A graph that retries the add would otherwise repeat the same line.
    //
    // Locked because the add runs on whichever thread kills the actor: a task pool worker as often as the main
    // thread, which is where the 2026-09-13 crash happened.
    static std::mutex s_reportedLock;
    static Set<void*> s_reported;

    std::scoped_lock _{s_reportedLock};

    if (s_reported.size() >= 256)
        s_reported.clear();

    if (s_reported.insert(apRagdoll).second)
    {
        // The actor lookup goes through the first body, so it is only attempted when the bodies passed.
        TESObjectREFR* const pReference = bodiesSound ? ResolveReference(apRagdoll) : nullptr;
        Actor* const pActor = Cast<Actor>(pReference);
        TESNPC* const pNpc = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
        ActorExtension* const pExtension = pActor ? pActor->GetExtension() : nullptr;
        const char* const cpName = pNpc && pNpc->fullName.value.AsAscii() ? pNpc->fullName.value.AsAscii() : "unknown";

        spdlog::critical("Ragdoll {} was not added to the physics world: {}. Reference {:X}, base {:X} ({}), race {:X}, {}, dead {}.", fmt::ptr(apRagdoll), cBadPart,
                         pReference ? pReference->formID : 0, pNpc ? pNpc->formID : 0, cpName, pNpc && pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0,
                         pExtension ? (pExtension->IsRemote() ? "remote" : "local") : "no extension", pActor && pActor->IsDead());

        // Flushed now: whatever let the list go bad may take the game down shortly after, and the line is the
        // whole point of this guard.
        spdlog::default_logger()->flush();
    }

    // What the original returns when the ragdoll is already in a world, so the caller treats it as nothing to do.
    return 1;
}

TiltedPhoques::Initializer s_ragdollAddGuard(
    []()
    {
        POINTER_SKYRIMSE(TAddToWorld, s_addToWorld, 64157);

        RealAddToWorld = s_addToWorld.Get();

        TP_HOOK(&RealAddToWorld, HookAddToWorld);
    });
} // namespace

#endif
