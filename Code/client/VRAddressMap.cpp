#include <TiltedOnlinePCH.h>

#include "VRAddressMap.h"

#if TP_SKYRIMVR

#include <algorithm>
#include <spdlog/spdlog.h>

namespace
{
#include "VRAddressMap.inl"

constexpr size_t kEntryCount = sizeof(kVRAddressMap) / sizeof(kVRAddressMap[0]);

const char* ConfidenceName(uint8_t aConfidence) noexcept
{
    switch (aConfidence)
    {
    case 5: return "override";
    case 4: return "curated-identical";
    case 3: return "curated-verified";
    case 2: return "curated-weak";
    case 1: return "auto-diff";
    default: return "addrlib";
    }
}
} // namespace

namespace VRAddresses
{
const VRAddress* Data() noexcept
{
    return kVRAddressMap;
}

size_t Count() noexcept
{
    return kEntryCount;
}

const VRAddress* Lookup(uint64_t aeId) noexcept
{
    if (aeId > UINT32_MAX)
        return nullptr;

    const VRAddress* pEnd = kVRAddressMap + kEntryCount;
    const VRAddress* pIt = std::lower_bound(kVRAddressMap, pEnd, static_cast<uint32_t>(aeId), [](const VRAddress& acEntry, uint32_t aId) { return acEntry.aeId < aId; });

    return (pIt != pEnd && pIt->aeId == aeId) ? pIt : nullptr;
}

void ReportMissing(uint64_t aeId) noexcept
{
    const VRAddress* pEntry = Lookup(aeId);

    char message[512];
    if (pEntry)
    {
        _snprintf_s(message, sizeof(message),
                    "Address Library id %llu (%s) has no SkyrimVR address yet.\n\n"
                    "Add a verified address to Tools/vr_addresses/overrides.csv, then re-run\n"
                    "    node Tools/vr_addresses/resolve.mjs",
                    aeId, pEntry->name);
    }
    else
    {
        _snprintf_s(message, sizeof(message),
                    "Address Library id %llu is not in the generated VR address table.\n\n"
                    "Re-run  node Tools/vr_addresses/resolve.mjs  to pick up newly added ids.",
                    aeId);
    }

    spdlog::critical("{}", message);
    if (spdlog::default_logger())
        spdlog::default_logger()->flush();

    OutputDebugStringA(message);
    OutputDebugStringA("\n");

    if (IsDebuggerPresent())
        __debugbreak();

    MessageBoxA(nullptr, message, "Skyrim Together VR", MB_OK | MB_ICONERROR);

    TerminateProcess(GetCurrentProcess(), 4);
    __assume(0);
}

void LogSummary() noexcept
{
    size_t byConfidence[6]{};
    size_t unmapped = 0;

    for (const VRAddress& entry : kVRAddressMap)
    {
        if (entry.vrOffset == 0)
            ++unmapped;
        else
            ++byConfidence[entry.confidence < 6 ? entry.confidence : 0];
    }

    spdlog::info("VR address table: {} ids, {} mapped, {} unmapped", kEntryCount, kEntryCount - unmapped, unmapped);
    for (int i = 5; i >= 0; --i)
    {
        if (byConfidence[i])
            spdlog::info("  {:<18} {}", ConfidenceName(static_cast<uint8_t>(i)), byConfidence[i]);
    }

    // Confidence 0 and 1 come from automated binary diffing, which is roughly 99.9% accurate.
    // Naming them here means a bad address has a short list of suspects instead of none.
    for (const VRAddress& entry : kVRAddressMap)
    {
        if (entry.vrOffset != 0 && entry.confidence <= 1)
            spdlog::debug("  unverified: id {} ({}) -> +0x{:X} [{}]", entry.aeId, entry.name, entry.vrOffset, ConfidenceName(entry.confidence));
    }
    for (const VRAddress& entry : kVRAddressMap)
    {
        if (entry.vrOffset == 0)
            spdlog::warn("  no VR address: id {} ({})", entry.aeId, entry.name);
    }
}
} // namespace VRAddresses

#endif
