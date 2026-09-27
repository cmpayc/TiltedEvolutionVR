#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/ActorSanity.h>
#include <Games/References.h>

#include <mutex>

namespace
{
// The game image, resolved once. A vtable pointer that does not land in here is not a vtable.
struct ImageRange
{
    uintptr_t Begin{0};
    uintptr_t End{0};
};

const ImageRange& GameImage() noexcept
{
    static const ImageRange s_range = []() -> ImageRange
    {
        auto* const pBase = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));

        if (!pBase)
            return {};

        const auto* const cpDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase);
        const auto* const cpNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(pBase + cpDos->e_lfanew);

        return {reinterpret_cast<uintptr_t>(pBase), reinterpret_cast<uintptr_t>(pBase) + cpNt->OptionalHeader.SizeOfImage};
    }();

    return s_range;
}

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

    // The region has to cover the whole span, not just its first byte.
    const auto cStart = reinterpret_cast<uintptr_t>(info.BaseAddress);

    return reinterpret_cast<uintptr_t>(apPtr) + aSize <= cStart + info.RegionSize;
}

/**
 * @brief Whether a value could be a pointer to a live object, judged without touching it.
 *
 * Null passes, because most of these fields are legitimately null. Everything else has to be 8 byte aligned,
 * below the canonical user space ceiling, and clear of the first 64KB, which is never mapped.
 */
bool LooksLikePointer(const uintptr_t aValue) noexcept
{
    if (aValue == 0)
        return true;

    return (aValue & 7) == 0 && aValue >= 0x10000 && aValue < 0x0000800000000000;
}

bool LooksLikeVTable(const uintptr_t aValue) noexcept
{
    const ImageRange& cImage = GameImage();

    return (aValue & 7) == 0 && aValue >= cImage.Begin && aValue < cImage.End;
}

/**
 * @brief Whether a value could point at a heap object, which is stricter than LooksLikePointer.
 *
 * A live object never lives inside the game image, so a field that is supposed to hold one and instead holds
 * an image address is wrong even though the value is a perfectly well formed pointer. That is exactly the
 * fourth crash of 2026-08-18: an AI package field held 0x14167CB80, the address of
 * VTABLE_BGSPackageDataBool_0, and the game called through it as though it were an instance.
 */
bool LooksLikeHeapPointer(const uintptr_t aValue) noexcept
{
    return LooksLikePointer(aValue) && !(aValue >= GameImage().Begin && aValue < GameImage().End);
}
} // namespace

bool ValidateActor(Actor* apActor, const char* acpWhere) noexcept
{
    if (!apActor)
        return true;

    // Called from the game thread and from the network threads, so the tables need a lock. Reported actors are
    // remembered so a broken one that the game keeps handing back does not fill the log.
    static std::mutex s_mutex;
    static Map<uintptr_t, uintptr_t> s_expectedOwner{};
    static Set<uint32_t> s_reported{};

    // Reading a freed actor must not be what takes the process down, so the span is probed first.
    if (!IsReadable(apActor, sizeof(Actor)))
    {
        std::scoped_lock lock(s_mutex);

        if (s_reported.insert(reinterpret_cast<uintptr_t>(apActor) & 0xFFFFFFFFu).second)
            spdlog::critical("ActorSanity: actor object at {} is not readable at {}, so it has been freed while something still held it", static_cast<void*>(apActor), acpWhere);

        return false;
    }

    const uint32_t cFormId = apActor->formID;
    const uintptr_t cClassVTable = *reinterpret_cast<const uintptr_t*>(apActor);
    const uintptr_t cOwnerVTable = *reinterpret_cast<const uintptr_t*>(&apActor->actorValueOwner);

    const char* pBadField = nullptr;
    size_t badOffset = 0;
    uintptr_t badValue = 0;

    const auto cCheck = [&](const bool aOk, const char* acpName, const size_t aOffset, const uintptr_t aValue)
    {
        if (!aOk && !pBadField)
        {
            pBadField = acpName;
            badOffset = aOffset;
            badValue = aValue;
        }
    };

    cCheck(LooksLikeVTable(cClassVTable), "class vtable", 0, cClassVTable);
    cCheck(LooksLikeVTable(cOwnerVTable), "ActorValueOwner vtable", offsetof(Actor, actorValueOwner), cOwnerVTable);
    cCheck(LooksLikeHeapPointer(reinterpret_cast<uintptr_t>(apActor->currentProcess)), "currentProcess", offsetof(Actor, currentProcess), reinterpret_cast<uintptr_t>(apActor->currentProcess));
    cCheck(LooksLikeHeapPointer(reinterpret_cast<uintptr_t>(apActor->pCombatController)), "pCombatController", offsetof(Actor, pCombatController), reinterpret_cast<uintptr_t>(apActor->pCombatController));
    cCheck(LooksLikeHeapPointer(reinterpret_cast<uintptr_t>(apActor->race)), "race", offsetof(Actor, race), reinterpret_cast<uintptr_t>(apActor->race));
    cCheck(LooksLikeHeapPointer(reinterpret_cast<uintptr_t>(apActor->equippedShout)), "equippedShout", offsetof(Actor, equippedShout), reinterpret_cast<uintptr_t>(apActor->equippedShout));

    // The ActorValueOwner vtable is constant per class, so the first actor of each class teaches what it is and
    // every later one is held to it. This is stricter than the range check above and catches a plausible
    // looking pointer that is simply the wrong object.
    bool ownerMismatch = false;
    uintptr_t ownerExpected = 0;

    {
        std::scoped_lock lock(s_mutex);

        if (LooksLikeVTable(cClassVTable) && LooksLikeVTable(cOwnerVTable))
        {
            const auto [entry, learned] = s_expectedOwner.try_emplace(cClassVTable, cOwnerVTable);

            if (learned)
                spdlog::info("ActorSanity: class vtable {:X} has ActorValueOwner vtable {:X} (learned at {}, actor {:X})", cClassVTable, cOwnerVTable, acpWhere, cFormId);
            else if (entry->second != cOwnerVTable)
            {
                ownerMismatch = true;
                ownerExpected = entry->second;
            }
        }

        if ((pBadField || ownerMismatch) && !s_reported.insert(cFormId).second)
            return false;
    }

    if (pBadField)
    {
        spdlog::critical("ActorSanity: actor {:X} is CORRUPT at {}: {} (+0x{:X}) holds {:X}, which cannot be a valid pointer", cFormId, acpWhere, pBadField, badOffset, badValue);
        return false;
    }

    if (ownerMismatch)
    {
        spdlog::critical("ActorSanity: actor {:X} is CORRUPT at {}: ActorValueOwner vtable is {:X}, expected {:X} for class vtable {:X}", cFormId, acpWhere, cOwnerVTable, ownerExpected, cClassVTable);
        return false;
    }

    return true;
}
