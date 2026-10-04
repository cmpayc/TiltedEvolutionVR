#include <TiltedOnlinePCH.h>
#include <Services/BodyPostPass.h>
#include <NetImmerse/BodyNative.h>

// The VR-only half of the post-pass re-apply: the three thunks on the BSFlattenedBoneTree downward-pass
// slots, their pinned install, and the per-second ownership check.
// Everything below is pinned to the Skyrim VR 1.4.15 image (sha 8daf0519…46d4) by RVA, byte signature and
// RTTI name, and refuses loudly on any mismatch: a refusal is today's writer, nothing else.

#if TP_SKYRIMVR

#include <chrono>
#include <cstring>

namespace BodyTracking::PostPass
{
namespace
{
constexpr uintptr_t kVtableRva = 0x17f19b8;  // VTABLE_BSFlattenedBoneTree_0 (id 286209)
constexpr uintptr_t kGetRttiRva = 0xcb0c10;  // slot 0x02: lea rax,[NiRTTI_BSFlattenedBoneTree]; ret
constexpr uintptr_t kNiRttiRva = 0x316be18;  // NiRTTI_BSFlattenedBoneTree (name pointer at +0)
constexpr std::array<size_t, 3> kSlotIndex{0x2d, 0x2e, 0x2f};
constexpr std::array<uintptr_t, 3> kWrapperRva{0xcb0030, 0xcb0060, 0xcb0090};
constexpr size_t kSignatureBytes = 27;
// push rbx; sub rsp,20; mov rbx,rcx; call <NiNode base pass>; mov rcx,rbx; add rsp,20; pop rbx; jmp <post-pass 0xcb0250>
constexpr std::array<std::array<uint8_t, kSignatureBytes>, 3> kSignatures{{
    {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xf2, 0xd6, 0xfe, 0xff, 0x48, 0x8b, 0xcb, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9, 0x05, 0x02, 0x00, 0x00},
    {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xc2, 0xd8, 0xfe, 0xff, 0x48, 0x8b, 0xcb, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9, 0xd5, 0x01, 0x00, 0x00},
    {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xe2, 0xdc, 0xfe, 0xff, 0x48, 0x8b, 0xcb, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9, 0xa5, 0x01, 0x00, 0x00}}};
constexpr const char* kRttiName = "BSFlattenedBoneTree";

using PassFn = void(__fastcall*)(void*, void*, uint32_t);

// Deliberately never deleted: a thunk may be inside Reapply during CRT teardown.
Registry* const g_registry = new Registry;
std::atomic<PassFn> g_original[3]{};
std::atomic<uintptr_t> g_slotAddress[3]{};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_lostLogged{false};
std::atomic<bool> g_active{false}; // cleared on the first detected slot loss: the remaining thunks stop re-applying

struct RawMemory
{
    template <class T> bool Load(uintptr_t aAddress, T& aValue) const noexcept
    {
        std::memcpy(&aValue, reinterpret_cast<const void*>(aAddress), sizeof(T));
        return true;
    }
    void Store(uintptr_t aAddress, const RawTransform& aValue) noexcept
    {
        std::memcpy(reinterpret_cast<void*>(aAddress), aValue.Bytes.data(), kTransformBytes);
    }
};

template <size_t I> void __fastcall Thunk(void* apTree, void* apData, uint32_t aFlags)
{
    const auto tree = reinterpret_cast<uintptr_t>(apTree);
    const auto token = g_registry->BeginPass(tree);
    g_original[I].load(std::memory_order_acquire)(apTree, apData, aFlags);
    if (token && g_active.load(std::memory_order_acquire))
    {
        const auto begin = std::chrono::steady_clock::now();
        RawMemory memory;
        ReapplyStats stats;
        if (g_registry->Reapply(tree, memory, stats, static_cast<uint32_t>(GetCurrentThreadId())))
            g_registry->NoteMicros(tree, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
    }
    g_registry->EndPass(token);
}
constexpr std::array<PassFn, 3> kThunks{&Thunk<0>, &Thunk<1>, &Thunk<2>};

struct Image
{
    uintptr_t Base{}, Size{};
};
Image GameImage() noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    return {base, nt->OptionalHeader.SizeOfImage};
}
bool Inside(const Image& aImage, uintptr_t aAddress, size_t aBytes) noexcept
{
    return aImage.Base && aAddress >= aImage.Base && aBytes <= aImage.Size && aAddress - aImage.Base <= aImage.Size - aBytes;
}
std::string ModuleOf(uintptr_t aAddress) noexcept
{
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(aAddress), &module))
        return "unknown";
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(module, path, MAX_PATH);
    std::string out;
    for (DWORD i = 0; i < length; ++i) out.push_back(path[i] < 128 ? static_cast<char>(path[i]) : '?');
    const auto slash = out.find_last_of("\\/");
    return slash == std::string::npos ? out : out.substr(slash + 1);
}
} // namespace

Registry& Global() noexcept
{
    return *g_registry;
}

bool Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

bool Active() noexcept
{
    return g_active.load(std::memory_order_acquire);
}

// Once per second from the update thread: are our three thunks still what the vtable holds?
bool SlotsIntact() noexcept
{
    if (!g_installed.load(std::memory_order_acquire)) return false;
    bool intact = true;
    for (size_t i = 0; i < 3; ++i)
    {
        const auto value = *reinterpret_cast<const volatile uintptr_t*>(g_slotAddress[i].load(std::memory_order_acquire));
        if (value == reinterpret_cast<uintptr_t>(kThunks[i])) continue;
        intact = false;
        g_active.store(false, std::memory_order_release); // one slot lost = today's writer on every slot
        if (!g_lostLogged.exchange(true))
            spdlog::warn("BODY POSTPASS lost slot={:x} now={:X} module={} reapply=disabled", kSlotIndex[i], value, ModuleOf(value));
    }
    return intact;
}

void RegisterAtStartup() noexcept
{
    static bool attempted{};
    if (attempted) return;
    attempted = true;
    const auto image = GameImage();
    const auto refuse = [](const char* apReason, const std::string& aDetail)
    {
        spdlog::warn("BODY POSTPASS refused reason={} {}", apReason, aDetail);
    };
    if (!image.Base) { refuse("no_image", ""); return; }
    const auto vtable = image.Base + kVtableRva;
    if (!Inside(image, vtable, (kSlotIndex[2] + 1) * sizeof(uintptr_t))) { refuse("vtable_outside_image", fmt::format("vtable={:X}", vtable)); return; }
    const auto slot2 = *reinterpret_cast<const uintptr_t*>(vtable + 2 * sizeof(uintptr_t));
    if (slot2 != image.Base + kGetRttiRva) { refuse("getrtti_slot", fmt::format("slot2={:X} expected={:X}", slot2, image.Base + kGetRttiRva)); return; }
    const auto rtti = image.Base + kNiRttiRva;
    if (!Inside(image, rtti, sizeof(uintptr_t))) { refuse("rtti_outside_image", ""); return; }
    const auto namePointer = *reinterpret_cast<const uintptr_t*>(rtti);
    if (!Inside(image, namePointer, 32) || std::strncmp(reinterpret_cast<const char*>(namePointer), kRttiName, 32) != 0)
    {
        refuse("rtti_name", fmt::format("name_ptr={:X}", namePointer));
        return;
    }
    for (size_t i = 0; i < 3; ++i)
    {
        const auto slotAddress = vtable + kSlotIndex[i] * sizeof(uintptr_t);
        const auto value = *reinterpret_cast<const uintptr_t*>(slotAddress);
        const auto expected = image.Base + kWrapperRva[i];
        if (value != expected)
        {
            refuse("slot_not_vanilla", fmt::format("slot={:x} value={:X} expected={:X} module={}", kSlotIndex[i], value, expected, ModuleOf(value)));
            return;
        }
        if (!Inside(image, expected, kSignatureBytes) || std::memcmp(reinterpret_cast<const void*>(expected), kSignatures[i].data(), kSignatureBytes) != 0)
        {
            refuse("wrapper_signature", fmt::format("slot={:x} at={:X}", kSlotIndex[i], expected));
            return;
        }
        g_slotAddress[i].store(slotAddress, std::memory_order_release);
    }
    // The originals are the validated vanilla wrappers: stored BEFORE any slot is exchanged, so a thunk that runs
    // mid-install always has a target.
    for (size_t i = 0; i < 3; ++i) g_original[i].store(reinterpret_cast<PassFn>(image.Base + kWrapperRva[i]), std::memory_order_release);
    const auto first = vtable + kSlotIndex[0] * sizeof(uintptr_t);
    const SIZE_T bytes = (kSlotIndex[2] - kSlotIndex[0] + 1) * sizeof(uintptr_t);
    DWORD previous{};
    if (!VirtualProtect(reinterpret_cast<void*>(first), bytes, PAGE_READWRITE, &previous))
    {
        refuse("virtualprotect", fmt::format("error={}", GetLastError()));
        return;
    }
    // Compare-and-swap each slot from its validated vanilla value to our thunk: a slot another patcher changed
    // between validation and here is never overwritten. Read back, and roll back only the slots this install
    // changed and that still hold our thunk, all while the page is writable.
    std::string failure;
    std::array<bool, 3> changed{};
    for (size_t i = 0; i < 3 && failure.empty(); ++i)
    {
        auto* slot = reinterpret_cast<void**>(g_slotAddress[i].load(std::memory_order_acquire));
        auto* expected = reinterpret_cast<void*>(image.Base + kWrapperRva[i]);
        const auto previous = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(kThunks[i]), expected);
        if (previous != expected) { failure = fmt::format("slot={:x} found={:X} module={}", kSlotIndex[i], reinterpret_cast<uintptr_t>(previous), ModuleOf(reinterpret_cast<uintptr_t>(previous))); break; }
        changed[i] = true;
        if (*reinterpret_cast<const volatile uintptr_t*>(slot) != reinterpret_cast<uintptr_t>(kThunks[i])) failure = fmt::format("slot={:x} readback", kSlotIndex[i]);
    }
    if (!failure.empty())
        for (size_t j = 0; j < 3; ++j)
            if (changed[j])
                InterlockedCompareExchangePointer(reinterpret_cast<void**>(g_slotAddress[j].load(std::memory_order_acquire)),
                    reinterpret_cast<void*>(g_original[j].load(std::memory_order_acquire)), reinterpret_cast<void*>(kThunks[j]));
    DWORD restored{};
    VirtualProtect(reinterpret_cast<void*>(first), bytes, previous, &restored);
    if (!failure.empty())
    {
        refuse("exchange", failure);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
    spdlog::info("BODY POSTPASS mode=on base={:X} vtable={:X} slots=2d,2e,2f originals={:X},{:X},{:X} moved_units={} max_slots={}",
        image.Base, vtable, reinterpret_cast<uintptr_t>(g_original[0].load()), reinterpret_cast<uintptr_t>(g_original[1].load()),
        reinterpret_cast<uintptr_t>(g_original[2].load()), kMovedUnits, kMaxSlots);
}
} // namespace BodyTracking::PostPass

#endif
