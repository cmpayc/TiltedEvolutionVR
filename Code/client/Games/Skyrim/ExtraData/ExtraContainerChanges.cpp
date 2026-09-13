#include <TiltedOnlinePCH.h>

#include <ExtraData/ExtraContainerChanges.h>

#if TP_SKYRIMVR
// GetArmor is reimplemented for VR below, which needs the armor form rather than a forward
// declaration of it.
#include <Forms/TESObjectARMO.h>
#endif

void ExtraContainerChanges::Data::Save(BGSSaveFormBuffer* apBuffer)
{
    TP_THIS_FUNCTION(TSaveFunc, void*, ExtraContainerChanges::Data, BGSSaveFormBuffer*);

    POINTER_SKYRIMSE(TSaveFunc, s_save, 16142);

    TiltedPhoques::ThisCall(s_save, this, apBuffer);
}

void ExtraContainerChanges::Data::Load(BGSLoadFormBuffer* apBuffer)
{
    TP_THIS_FUNCTION(TLoadFunc, void*, ExtraContainerChanges::Data, BGSLoadFormBuffer*);

    POINTER_SKYRIMSE(TLoadFunc, s_load, 16143);

    TiltedPhoques::ThisCall(s_load, this, apBuffer);
}

bool ExtraContainerChanges::Entry::IsQuestObject() noexcept
{
    TP_THIS_FUNCTION(TIsQuestObject, bool, ExtraContainerChanges::Entry);

    POINTER_SKYRIMSE(TIsQuestObject, s_isQuestObject, 16005);

    return TiltedPhoques::ThisCall(s_isQuestObject, this);
}

TESObjectARMO* ExtraContainerChanges::Data::GetArmor(uint32_t aSlotId) noexcept
{
#if TP_SKYRIMVR
    // SkyrimVR 1.4.15 has no standalone copy of this function. AE keeps it as one 0x4A-byte
    // function with a single caller; VR's compiler inlined it there, so Address Library id 16113
    // has nothing to map to and asking for it is a hard failure. PROGRESS.md records the evidence.
    //
    // So it is reimplemented rather than left to fail, and it computes the same answer from data
    // the client already models. AE's version rejects a slot below 30, indexes the biped slot mask
    // with slotId - 30, and returns whichever worn armor occupies that slot. The mask lives in
    // TESObjectARMO::slotType, which is what the IsBodyPiece() next door already reads, and the
    // worn flags are the ones GetInventory() already reads out of each entry's extra data.
    if (aSlotId < 30 || !entries)
        return nullptr;

    const uint32_t slotMask = 1u << (aSlotId - 30);

    for (Entry* pEntry : *entries)
    {
        if (!pEntry || !pEntry->form || pEntry->form->formType != FormType::Armor || !pEntry->dataList)
            continue;

        auto* pArmor = Cast<TESObjectARMO>(pEntry->form);
        if (!pArmor || !(pArmor->slotType & slotMask))
            continue;

        for (ExtraDataList* pExtraDataList : *pEntry->dataList)
        {
            if (!pExtraDataList)
                continue;

            if (pExtraDataList->Contains(ExtraDataType::Worn) || pExtraDataList->Contains(ExtraDataType::WornLeft))
                return pArmor;
        }
    }

    return nullptr;
#else
    TP_THIS_FUNCTION(TGetArmor, TESObjectARMO*, ExtraContainerChanges::Data, uint32_t);

    POINTER_SKYRIMSE(TGetArmor, s_getArmor, 16113);

    return TiltedPhoques::ThisCall(s_getArmor, this, aSlotId);
#endif
}
