#pragma once

#include <Forms/TESForm.h>

struct TESObjectREFR;
struct TESWorldSpace;
struct BGSEncounterZone;
struct LoadedCellData;

struct TESObjectCELL : TESForm
{
    Vector<TESObjectREFR*> GetRefsByFormTypes(const Vector<FormType>& aFormTypes) const noexcept;
    void GetCOCPlacementInfo(NiPoint3* aOutPos, NiPoint3* aOutRot, bool aAllowCellLoad) noexcept;

    bool IsValid() const { return cellFlags[4] == 7; }

    struct ReferenceData
    {
        struct Reference
        {
            TESObjectREFR* ref;
            void* unk08;

            TESObjectREFR* Get() { return unk08 != nullptr ? ref : nullptr; }
        };

        uint64_t unk0;
        uint32_t unk8;
        uint32_t capacity;
        uint32_t available;
        uint32_t unkC;
        void* unk10;
        void* unk18;
        Reference* refArray;

        uint32_t Count() { return capacity - available; }
    };
    static_assert(sizeof(ReferenceData) == 0x30);

    struct LoadedCellData
    {
        uint8_t pad0[0x160];
        BGSEncounterZone* encounterZone;
    };
    static_assert(offsetof(LoadedCellData, encounterZone) == 0x160);

    uint8_t pad20[0x40 - 0x20];
    uint8_t cellFlags[5];
    bool autoWaterLoaded;
    bool cellDetached;
    uint8_t pad47;
    ExtraDataList extraData;
    uint64_t cellData;
    void* pCellLand;
    float waterHeight;
    void* pNavMeshes;
    ReferenceData refData;
    void* pUnkB8;
    GameArray<TESObjectREFR*> objectList;
    GameArray<void*> unkD8;
    GameArray<void*> unkF0;
    GameArray<void*> unk108;
    BSRecursiveLock lock;
    TESWorldSpace* worldspace;
    LoadedCellData* loadedCellData;
    void* pLightingTemplate;
    uint64_t unk140;
};

// extraData sits at 0x48 in both builds, so everything after it is 8 bytes lower on VR. See
// ExtraDataList.h. Confirmed three ways against SkyrimVR: the constructors match field for field
// with a uniform -8 from SE 0x68 onwards (SE 0x1402B2070, VR 0x140270790), TESWorldSpace::LoadCell
// reads the worldspace with a byte-identical instruction bar the displacement (SE 0x140306C86
// `mov r15,[rax+0x128]`, VR 0x1402C3AA1 `mov r15,[rax+0x120]`), and the cell each LoadCell
// allocates is 0x148 bytes on SE and 0x140 on VR.
static_assert(offsetof(TESObjectCELL, cellFlags) == 0x40);
static_assert(offsetof(TESObjectCELL, refData) == 0x88 - kExtraDataListDelta);
static_assert(offsetof(TESObjectCELL, worldspace) == 0x128 - kExtraDataListDelta);
static_assert(offsetof(TESObjectCELL, loadedCellData) == 0x130 - kExtraDataListDelta);
static_assert(sizeof(TESObjectCELL) == 0x148 - kExtraDataListDelta);
