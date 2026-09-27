#pragma once

#include "ExtraData.h"

#include <ExtraData/ExtraSoul.h>

struct AlchemyItem;
struct EnchantmentItem;

struct ExtraDataList
{
    static ExtraDataList* New() noexcept;

    bool Contains(ExtraDataType aType) const;
    void Set(ExtraDataType aType, bool aSet);

    bool Add(ExtraDataType aType, BSExtraData* apNewData);
    bool Remove(ExtraDataType aType, BSExtraData* apNewData);

    uint32_t GetCount() const;

    void SetType(ExtraDataType aType, bool aClear);
    BSExtraData* GetByType(ExtraDataType type) const;

    void SetSoulData(SOUL_LEVEL aSoulLevel) noexcept;
    void SetChargeData(float aCharge) noexcept;
    void SetWorn(bool aWornLeft) noexcept;
    void SetPoison(AlchemyItem* apItem, uint32_t aCount) noexcept;
    void SetHealth(float aHealth) noexcept;
    void SetEnchantmentData(EnchantmentItem* apItem, uint16_t aCharge, bool aRemoveOnUnequip) noexcept;

    [[nodiscard]] bool HasQuestObjectAlias() noexcept;

#if !TP_SKYRIMVR
    // Anniversary Edition gave ExtraDataList a virtual destructor. SkyrimVR 1.4.15 has no vtable
    // on it at all, which both constructors state outright:
    //   SE 0x140151E90  lea rax,[rip+vtable] / mov [rcx],rax / xor eax,eax / mov [rcx+8],rax
    //   VR 0x140117C80  xor eax,eax / mov [rcx],rax / mov [rcx+8],rax / add rcx,0x10 / call lock
    // So SE is {vtable 0x00, data 0x08, bitfield 0x10, lock 0x18} at 0x20 bytes, and VR is
    // {data 0x00, bitfield 0x08, lock 0x10} at 0x18.
    //
    // This one difference is where every 8-byte layout delta on the form path comes from, because
    // TESObjectREFR and TESObjectCELL both hold one by value:
    //   TESObjectREFR  0xA0  -> 0x98      (extraData at 0x70)
    //   TESObjectCELL  0x148 -> 0x140     (extraData at 0x48, worldspace 0x128 -> 0x120)
    //   Character      0x2B8 -> 0x2B0     (so Actor::flags1 0xE8 -> 0xE0)
    // The destructor is declared and never defined; it only ever existed to model the slot.
    virtual ~ExtraDataList();
#endif
    BSExtraData* data = nullptr;

    struct Bitfield
    {
        uint8_t data[0x18];
    };

    Bitfield* bitfield{};
    mutable BSRecursiveLock lock{};
};

// Members that sit past an ExtraDataList held by value are this much lower on VR. Subtracting it
// in the asserts below keeps the SE offsets readable and states the relationship in one place.
constexpr size_t kExtraDataListDelta = TP_SKYRIMVR ? 8 : 0;

static_assert(sizeof(ExtraDataList) == 0x20 - kExtraDataListDelta);
static_assert(offsetof(ExtraDataList, data) == 0x8 - kExtraDataListDelta);
static_assert(offsetof(ExtraDataList, bitfield) == 0x10 - kExtraDataListDelta);
static_assert(offsetof(ExtraDataList, lock) == 0x18 - kExtraDataListDelta);
