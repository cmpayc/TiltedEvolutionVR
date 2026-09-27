#include <TiltedOnlinePCH.h>

#include <ModCompat/HiggsAPI.h>

#if TP_SKYRIMVR

namespace
{
constexpr char kHiggsModule[] = "higgs_vr.dll";

// RTTI type descriptor name of HIGGS's concrete interface class, the one statically initialised
// object its SKSE reply hands out. The whole walk keys off this string rather than off offsets, so
// it re-derives itself on any HIGGS build that keeps RTTI on.
//
// Note this is the concrete HiggsInterface001, not the abstract IHiggsInterface001, whose
// descriptor sits alongside it and has no instance.
constexpr char kInterfaceTypeName[] = ".?AUHiggsInterface001@HiggsPluginAPI@@";

struct Section
{
    uint8_t* pBegin{nullptr};
    uint8_t* pEnd{nullptr};
};

bool FindSection(uint8_t* apBase, const char* acpName, Section& aOut) noexcept
{
    const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(apBase);
    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(apBase + pDos->e_lfanew);

    if (pNt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto* pSection = IMAGE_FIRST_SECTION(pNt);

    for (uint16_t i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSection)
    {
        char name[9]{};
        std::memcpy(name, pSection->Name, 8);

        if (std::strcmp(name, acpName) != 0)
            continue;

        aOut.pBegin = apBase + pSection->VirtualAddress;
        aOut.pEnd = aOut.pBegin + pSection->Misc.VirtualSize;

        return true;
    }

    return false;
}

IHiggsInterface001* LocateInterface(uint8_t* apBase) noexcept
{
    Section data{};
    Section rdata{};

    if (!FindSection(apBase, ".data", data) || !FindSection(apBase, ".rdata", rdata))
    {
        spdlog::error("HIGGS: {} is missing .data or .rdata, cannot locate the interface", kHiggsModule);
        return nullptr;
    }

    // MSVC emits RTTI type descriptors into .data. The name sits 0x10 into the descriptor, past the
    // type_info vftable pointer and the spare field. Matching includes the terminator so a longer
    // name that merely starts the same cannot match.
    uint8_t* pTypeName = nullptr;
    for (uint8_t* p = data.pBegin; p + sizeof(kInterfaceTypeName) <= data.pEnd; ++p)
    {
        if (std::memcmp(p, kInterfaceTypeName, sizeof(kInterfaceTypeName)) == 0)
        {
            pTypeName = p;
            break;
        }
    }

    if (!pTypeName)
    {
        spdlog::error("HIGGS: type descriptor '{}' not found in {}, is it built without RTTI?", kInterfaceTypeName, kHiggsModule);
        return nullptr;
    }

    const auto cTypeDescriptorRva = static_cast<uint32_t>(pTypeName - 0x10 - apBase);

    // The complete object locator carries signature 1 on x64 and names its type descriptor at +0x0C.
    uint8_t* pLocator = nullptr;
    for (uint8_t* p = rdata.pBegin; p + 0x18 <= rdata.pEnd; p += 4)
    {
        if (*reinterpret_cast<const uint32_t*>(p) == 1 && *reinterpret_cast<const uint32_t*>(p + 0x0C) == cTypeDescriptorRva)
        {
            pLocator = p;
            break;
        }
    }

    if (!pLocator)
    {
        spdlog::error("HIGGS: no complete object locator references type descriptor at rva {:#x}", cTypeDescriptorRva);
        return nullptr;
    }

    // A vtable is preceded by a pointer back to its locator, so the table itself starts 8 bytes on.
    const auto cLocatorAddress = reinterpret_cast<uintptr_t>(pLocator);

    uint8_t* pVTable = nullptr;
    for (uint8_t* p = rdata.pBegin; p + 8 <= rdata.pEnd; p += 8)
    {
        if (*reinterpret_cast<const uintptr_t*>(p) == cLocatorAddress)
        {
            pVTable = p + 8;
            break;
        }
    }

    if (!pVTable)
    {
        spdlog::error("HIGGS: no vtable references the complete object locator at {:#x}", cLocatorAddress);
        return nullptr;
    }

    // HIGGS's interface is a single statically initialised global, so its vtable pointer is already
    // baked into .data and one scan finds it. The shipped v1.10.10 has exactly one such object.
    const auto cVTableAddress = reinterpret_cast<uintptr_t>(pVTable);

    for (uint8_t* p = data.pBegin; p + 8 <= data.pEnd; p += 8)
    {
        if (*reinterpret_cast<const uintptr_t*>(p) == cVTableAddress)
            return reinterpret_cast<IHiggsInterface001*>(p);
    }

    spdlog::error("HIGGS: found the vtable at {:#x} but no instance of it in .data", cVTableAddress);

    return nullptr;
}
} // namespace

IHiggsInterface001* HiggsAPI::Acquire() noexcept
{
    const HMODULE cHiggs = GetModuleHandleA(kHiggsModule);
    if (!cHiggs)
        return nullptr;

    IHiggsInterface001* pInterface = LocateInterface(reinterpret_cast<uint8_t*>(cHiggs));
    if (!pInterface)
        return nullptr;

    // Proves the pointer is callable and that slot 0 really is GetBuildNumber. HIGGS packs its
    // version one field per two decimal digits, so 1101000 reads as 1.10.10.
    const uint32_t cBuild = pInterface->GetBuildNumber();
    if (cBuild == 0)
    {
        spdlog::error("HIGGS: interface at {:#x} reports build 0, the vtable walk landed somewhere wrong", reinterpret_cast<uintptr_t>(pInterface));
        return nullptr;
    }

    spdlog::info("HIGGS v{}.{}.{} found, interface at {:#x}", cBuild / 1000000, (cBuild / 10000) % 100, (cBuild / 100) % 100, reinterpret_cast<uintptr_t>(pInterface));

    return pInterface;
}

#endif
