#include <TiltedOnlinePCH.h>

#include <Games/Memory.h>
#include <Games/References.h>

#include <TiltedCore/MimallocAllocator.hpp>
#include <mimalloc.h>

#pragma optimize("", off)

struct GameHeap
{
    static GameHeap* Get()
    {
        POINTER_SKYRIMSE(GameHeap, s_gameHeap, 400188);

        return s_gameHeap.Get();
    }
};

TP_THIS_FUNCTION(TFormAllocate, void*, GameHeap, size_t aSize, size_t aAlignment, bool aAligned);
TP_THIS_FUNCTION(TFormFree, void, GameHeap, void* apPtr, bool aAligned);

TFormAllocate* RealFormAllocate = nullptr;
TFormFree* RealFormFree = nullptr;

static TiltedPhoques::MimallocAllocator s_allocator;

namespace
{
/**
 * @brief Reports whether the enlargement below ever actually fires.
 *
 * It is keyed on an exact size match, so if the game's real Actor allocation is not sizeof(Actor) then
 * no block is ever enlarged, yet Actor::GetExtension() still casts to ExActor* and writes at
 * sizeof(Actor) and beyond, past the end of the object and into the next heap block. That is the
 * leading suspect for the 2026-08-18 crash, where one actor's ActorValueOwner vtable pointer held a
 * heap pointer. If "enlarged an actor allocation" never appears in the log while actors exist, the
 * hook is dead and that is the bug.
 *
 * Runs on every allocation and on many threads, so it uses atomics only: no containers, no locks.
 */
void ReportActorAllocation(size_t aRequested, bool aEnlarged) noexcept
{
    static std::atomic<bool> s_announced{false};
    static std::atomic<bool> s_enlargedOnce{false};
    static std::atomic<uint32_t> s_nearMisses{0};

    if (!s_announced.exchange(true))
        spdlog::info("Memory hook: sizeof(Actor)={:X}, sizeof(ExActor)={:X}, sizeof(PlayerCharacter)={:X}, sizeof(ExPlayerCharacter)={:X}", sizeof(Actor), sizeof(ExActor), sizeof(PlayerCharacter), sizeof(ExPlayerCharacter));

    if (aEnlarged)
    {
        if (!s_enlargedOnce.exchange(true))
            spdlog::info("Memory hook: enlarged an actor allocation for the first time, requested {:X}", aRequested);

        return;
    }

    // A request that lands near sizeof(Actor) without matching it is the signature of a struct whose
    // size is wrong for this build. Capped so a busy allocator cannot flood the log.
    const bool cNear = (aRequested + 0x20 > sizeof(Actor) && aRequested < sizeof(Actor) + 0x20) || (aRequested + 0x20 > sizeof(PlayerCharacter) && aRequested < sizeof(PlayerCharacter) + 0x20);

    if (cNear && s_nearMisses.fetch_add(1) < 12)
        spdlog::warn("Memory hook: allocation of {:X} was NOT enlarged, but sizeof(Actor)={:X} and sizeof(PlayerCharacter)={:X}", aRequested, sizeof(Actor), sizeof(PlayerCharacter));
}
} // namespace

void* TP_MAKE_THISCALL(HookFormAllocate, GameHeap, size_t aSize, size_t aAlignment, bool aAligned)
{
    const size_t cRequested = aSize;

    switch (aSize)
    {
    case sizeof(Actor): aSize = sizeof(ExActor); break;
    case sizeof(PlayerCharacter): aSize = sizeof(ExPlayerCharacter); break;
    default: break;
    }

    ReportActorAllocation(cRequested, aSize != cRequested);

    auto* pPointer = TiltedPhoques::ThisCall(RealFormAllocate, apThis, aSize, aAlignment, aAligned);

    if (!pPointer)
        return nullptr;

    ActorExtension* pExtension = nullptr;

    switch (aSize)
    {
    case sizeof(ExActor): pExtension = static_cast<ActorExtension*>(static_cast<ExActor*>(pPointer)); break;
    case sizeof(ExPlayerCharacter): pExtension = static_cast<ActorExtension*>(static_cast<ExPlayerCharacter*>(pPointer)); break;
    default: break;
    }

    if (pExtension)
    {
        new (pExtension) ActorExtension;
    }

    return pPointer;
}

void* Memory::Allocate(const size_t aSize) noexcept
{
    return TiltedPhoques::ThisCall(HookFormAllocate, GameHeap::Get(), aSize, 0, false);
}

void Memory::Free(void* apData) noexcept
{
    TiltedPhoques::ThisCall(RealFormFree, GameHeap::Get(), apData, false);
}

static bool IsFormAllocateReplacedByEF(TFormAllocate** appOutEngineFixesAlloc) noexcept
{
    POINTER_SKYRIMSE(TFormAllocate, s_formAllocate, 68115);
    TFormAllocate* pFormAllocate = s_formAllocate.Get();

    auto opcodeBytes = reinterpret_cast<uint16_t*>(*pFormAllocate);
    uint8_t shift = 0;

    if (*opcodeBytes == 0x25FF) // 'jmp' opcode 'FF 25' and the 4-byte displacement bytes come before the virtual address we're after
        shift = 6;
    else if (*opcodeBytes == 0xB848) // 'mov' opcode '48 B8' comes before the virtual address we're after
        shift = 2;

    auto possibleEfAllocAddress = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(*pFormAllocate) + shift);

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)possibleEfAllocAddress, &mbi, sizeof(mbi)) != 0)
    {
        if (mbi.AllocationBase == GetModuleHandleW(L"EngineFixes.dll"))
        {
            *appOutEngineFixesAlloc = reinterpret_cast<TFormAllocate*>(possibleEfAllocAddress);
            return true;
        }
    }
    return false;
}

static void RehookFormAllocate(TFormAllocate* apEngineFixesAllocate) noexcept
{
    RealFormAllocate = apEngineFixesAllocate;
    TP_HOOK_IMMEDIATE(&RealFormAllocate, HookFormAllocate);
}

size_t Hook_msize(void* apData)
{
    return mi_malloc_size(apData);
}

void Hookfree(void* apData)
{
    mi_free(apData);
}

void* Hookcalloc(size_t aCount, size_t aSize)
{
    return mi_calloc(aCount, aSize);
}

void* Hookmalloc(size_t aSize)
{
    return mi_malloc(aSize);
}

void Hook_aligned_free(void* apData)
{
    mi_free(apData);
}

void* Hook_aligned_malloc(size_t aSize, size_t aAlignment)
{
    return mi_malloc_aligned(aSize, aAlignment);
}

static TiltedPhoques::Initializer s_memoryHooks(
    []()
    {
        POINTER_SKYRIMSE(TFormAllocate, s_formAllocate, 68115);

        POINTER_SKYRIMSE(TFormFree, s_formFree, 68117);

        RealFormAllocate = s_formAllocate.Get();
        RealFormFree = s_formFree.Get();

        using T_msize = decltype(&Hook_msize);
        using Tfree = decltype(&Hookfree);
        using Tcalloc = decltype(&Hookcalloc);
        using Tmalloc = decltype(&Hookmalloc);
        using T_aligned_malloc = decltype(&Hook_aligned_malloc);
        using T_aligned_free = decltype(&Hook_aligned_free);
        T_msize Real_msize = nullptr;
        Tfree Realfree = nullptr;
        Tcalloc Realcalloc = nullptr;
        Tmalloc Realmalloc = nullptr;
        T_aligned_malloc Real_aligned_malloc = nullptr;
        T_aligned_free Real_aligned_free = nullptr;

        const char* cModuleName = "api-ms-win-crt-heap-l1-1-0.dll";

        TP_HOOK_IAT(_msize, cModuleName);
        TP_HOOK_IAT(free, cModuleName);
        TP_HOOK_IAT(calloc, cModuleName);
        TP_HOOK_IAT(malloc, cModuleName);
        TP_HOOK_IAT(_aligned_malloc, cModuleName);
        TP_HOOK_IAT(_aligned_free, cModuleName);

        TP_HOOK(&RealFormAllocate, HookFormAllocate);
    });

using T_initterm_e = decltype(&_initterm_e);
T_initterm_e Real_initterm_e = nullptr;

// If EngineFixes loaded, and it changed our FormAllocate hook, 
// reset it. Our hook works just fine chaining to theirs.
int __cdecl Hook_initterm_e(_PIFV* apFirst, _PIFV* apLast)
{
    // We want to run last, so pre-chain.
    auto retval = Real_initterm_e(apFirst, apLast); 
    
    // Check if EngineFixes messed with STR's modified alloc hook; if it did, treat EF as truth and rehook
    TFormAllocate* pEngineFixesAllocate = nullptr;
    if (GetModuleHandleW(L"EngineFixes.dll") && IsFormAllocateReplacedByEF(&pEngineFixesAllocate))
        RehookFormAllocate(pEngineFixesAllocate);

    return retval;
}

void HookFormAllocateSentinelInit()
{
    TP_HOOK_IAT(_initterm_e, "api-ms-win-crt-runtime-l1-1-0.dll");
}
#pragma optimize("", on)
