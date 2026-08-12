#pragma once

#include <Games/Primitives.h>
#include <Forms/TESForm.h>
#include <NetImmerse/BSFaceGenNiNode.h>
#include "ExtraData.h"
#include <ExtraData/ExtraContainerChanges.h>
#include <Games/Animation/IAnimationGraphManagerHolder.h>
#include <Games/Misc/Lock.h>
#include <Games/Magic/MagicSystem.h>
#include <Magic/MagicCaster.h>
#include <Magic/MagicTarget.h>
#include <Structs/Inventory.h>
#include <ExtraData/ExtraDataList.h>

struct AnimationVariables;
struct TESWorldSpace;
struct TESBoundObject;
struct TESContainer;

enum class ITEM_REMOVE_REASON
{
    kRemove,
    kSteal,
    kSelling,
    kDropping,
    kStoreInContainer,
    kStoreInTeammate
};

struct TESObjectREFR : TESForm
{
    enum ChangeFlags : uint32_t
    {
        CHANGE_REFR_MOVE = 1 << 1,
        CHANGE_REFR_HAVOK_MOVE = 1 << 2,
        CHANGE_REFR_CELL_CHANGED = 1 << 3,
        CHANGE_REFR_SCALE = 1 << 4,
        CHANGE_REFR_INVENTORY = 1 << 5,
        CHANGE_REFR_EXTRA_OWNERSHIP = 1 << 6,
        CHANGE_REFR_BASEOBJECT = 1 << 7,
        CHANGE_REFR_PROMOTED = 1 << 25,
        CHANGE_REFR_EXTRA_ACTIVATING_CHILDREN = 1 << 26,
        CHANGE_REFR_LEVELED_INVENTORY = 1 << 27,
        CHANGE_REFR_ANIMATION = 1 << 28,
        CHANGE_REFR_EXTRA_ENCOUNTER_ZONE = 1 << 29,
        CHANGE_REFR_EXTRA_CREATED_ONLY = 1 << 30,
        CHANGE_REFR_EXTRA_GAME_ONLY = 1u << 31,
    };

    enum OpenState : uint8_t
    {
        kNone = 0,
        kOpen,
        kOpening,
        kClosed,
        kClosing,
    };

    static TESObjectREFR* GetByHandle(uint32_t aHandle) noexcept;

    /**
     * @brief Resolves a handle without releasing it.
     *
     * GetByHandle calls DecRefHandle, which destroys the object once the count reaches zero. That is right
     * for a handle you own and wrong for one belonging to somebody else.
     *
     * Use this to look at a reference the game has just handed back and still owns, such as the object a
     * drop creates. Releasing that one leaves a reference that still exists and can still be grabbed but no
     * longer moves or reports its position, which looked exactly like a sync fault.
     */
    static TESObjectREFR* PeekByHandle(uint32_t aHandle) noexcept;
    static uint32_t* GetNullHandle() noexcept;

    static void GetItemFromExtraData(Inventory::Entry& arEntry, ExtraDataList* apExtraDataList) noexcept;
    static ExtraDataList* GetExtraDataFromItem(const Inventory::Entry& arEntry) noexcept;

    virtual void sub_3B();
    virtual void sub_3C();
    virtual void sub_3D();
    virtual void sub_3E();
    virtual void Update3DPosition(bool abWarp);
    virtual void sub_40();
    virtual void sub_41();
    virtual void sub_42();
    virtual void sub_43();
    virtual void sub_44();
    virtual void sub_45();
    virtual void sub_46();
    virtual void sub_47();
    virtual void sub_48();
    virtual void sub_49();
    virtual void sub_4A();
    virtual void sub_4B();
    virtual void sub_4C();
    virtual void sub_4D();
    virtual void sub_4E();
    virtual void StopCurrentDialogue(bool aForce);
    virtual void sub_50();
    virtual void sub_51();
    virtual void sub_52();
    virtual void sub_53();
    virtual void sub_54();
    virtual void sub_55();
    virtual BSPointerHandle<TESObjectREFR> RemoveItem(TESBoundObject* apItem, int32_t aCount, ITEM_REMOVE_REASON aReason, ExtraDataList* apExtraList, TESObjectREFR* apMoveToRef, const NiPoint3* apDropLoc = nullptr, const NiPoint3* apRotate = nullptr);
    virtual void sub_57();
    virtual void sub_58();
    virtual void sub_59();
    virtual void AddObjectToContainer(TESBoundObject* apObj, ExtraDataList* aspExtra, int32_t aicount, TESObjectREFR* apOldContainer);
    virtual void sub_5B();
    virtual MagicCaster* GetMagicCaster(MagicSystem::CastingSource aeSource);
    virtual MagicTarget* GetMagicTarget();
    virtual void sub_5E();
    virtual void sub_5F();
    virtual void sub_60();
    virtual BSFaceGenNiNode* GetFaceGenNiNode();
    virtual void sub_62();
    virtual void sub_63();
    virtual void sub_64();
    virtual void DetachHavok();
    virtual void sub_66();
    virtual void sub_67();
    virtual void sub_68();
    virtual void sub_69();
    virtual void sub_6A();
    virtual void sub_6B();
    virtual void sub_6C();
    virtual void sub_6D();
    virtual void sub_6E();
    virtual void sub_6F();
    virtual NiNode* GetNiNode();
    virtual void sub_71();
    virtual void sub_72();
    virtual NiPoint3 GetBoundMin();
    virtual NiPoint3 GetBoundMax();
    virtual void sub_75();
    virtual void sub_76();
    virtual void sub_77();
    virtual void sub_78();
    virtual void sub_79();
    virtual void sub_7A();
    virtual void sub_7B();
    virtual void sub_7C();
    virtual void sub_7D();
    virtual void* sub_7E(bool aUnk);
    virtual void sub_7F();
    virtual void sub_80();
    virtual void sub_81();
#if TP_SKYRIMVR
    // SkyrimVR has one more virtual than 1.6.1170 at exactly this point, so every slot from here on
    // is one later on VR. Established by comparing the two Character vtables slot by slot, with each
    // SE target projected into VR through the ordinary id chain: slots up to 0x81 match at the same
    // index, and from 0x82 onwards VR slot N holds what SE slot N-1 holds, unbroken for the whole
    // remaining stretch. The primary vtables are SE 0x1418A5558 and VR 0x1416D6DE0.
    //
    // Without this placeholder every virtual declared below dispatches one slot early. That is how
    // GetParentCell came to return 1: it was calling VR's slot 0x97, which is SE's 0x96.
    virtual void vrExtraSlot_81();
#endif
    virtual void sub_82();
    virtual void sub_83();
    virtual void SetBaseForm(TESForm* apForm);
    virtual void sub_85();
    virtual void sub_86();
    virtual void sub_87();
    virtual void sub_88();
    virtual void DisableImpl();
    virtual void ResetInventory(bool abLeveledOnly);
    virtual void sub_8B();
    virtual void sub_8C();
    virtual void sub_8D();
    virtual void sub_8E();
    virtual void sub_8F();
    virtual void sub_90();
    virtual void sub_91();
    virtual void sub_92();
    virtual void sub_93();
    virtual void sub_94();
    virtual void sub_95();
    virtual void sub_96();
    virtual struct TESObjectCELL* GetParentCell() const;
    virtual void SetParentCell(struct TESObjectCELL* apParentCell);
    virtual bool VirtIsDead(bool abNotEssential); // TODO: Use this or papyrus IsDead?
    virtual void sub_9A();
    virtual void sub_9B();

    void SetRotation(float aX, float aY, float aZ) noexcept;

    BSPointerHandle<TESObjectREFR> GetHandle() const noexcept;
    uint32_t GetCellId() const noexcept;
    TESWorldSpace* GetWorldSpace() const noexcept;
    ExtraContainerChanges::Data* GetContainerChanges() const noexcept;
    ExtraDataList* GetExtraDataList() noexcept;
    Lock* GetLock() const noexcept;
    TESContainer* GetContainer() const noexcept;
    int64_t GetItemCountInInventory(TESForm* apItem) const noexcept;
    TESObjectCELL* GetParentCellEx() const noexcept;

    void SaveAnimationVariables(AnimationVariables& aWriter) const noexcept;
    void LoadAnimationVariables(const AnimationVariables& aReader) const noexcept;
    uint32_t GetAnimationVariableInt(BSFixedString* apVariableName) noexcept;

    void RemoveAllItems() noexcept;
    Vector<uint32_t> RemoveNonQuestItems(Inventory& aCurrentInventory) noexcept;
    void Delete() const noexcept;
    void Disable() const noexcept;
    void Enable() const noexcept;
    void MoveTo(TESObjectCELL* apCell, const NiPoint3& acPosition) const noexcept;
    void PayGold(int32_t aAmount) noexcept;
    void PayGoldToContainer(TESObjectREFR* pContainer, int32_t aAmount) noexcept;
    bool SendAnimationEvent(BSFixedString* apEventName) noexcept;

    bool Activate(TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* apObjectToGet, int32_t aCount, char aDefaultProcessing) noexcept;

    bool PlayAnimationAndWait(BSFixedString* apAnimation, BSFixedString* apEventName) noexcept;
    bool PlayAnimation(BSFixedString* apEventName) noexcept;

    Lock* CreateLock() noexcept;
    void LockChange() noexcept;

    const float GetHeight() noexcept;
    void EnableImpl() noexcept;
    OpenState GetOpenState() noexcept;

    Inventory GetInventory() const noexcept;
    Inventory GetInventory(std::function<bool(TESForm&)> aFilter) const noexcept;
    Inventory GetArmor() const noexcept;
    Inventory GetWornArmor() const noexcept;

    bool IsItemInInventory(uint32_t aFormID) const noexcept;

    void SetInventory(const Inventory& acContainer) noexcept;
    void SetInventoryRetainingQuestItems(Inventory& aCurrentInventory, const Inventory& acSourceInventory) noexcept;
    void AddOrRemoveItem(const Inventory::Entry& arEntry, bool aIsSettingInventory = false) noexcept;
    void UpdateItemList(TESForm* pUnkForm) noexcept;

    BSHandleRefObject handleRefObject;
    uintptr_t unk1C;
    IAnimationGraphManagerHolder animationGraphHolder;
    TESForm* baseForm; // 40 SE
    NiPoint3 rotation;
    NiPoint3 position;
    TESObjectCELL* parentCell;
    void* loadedState;
    ExtraDataList extraData;
    BSRecursiveLock refLock;
    uint16_t scale;
    uint16_t referenceFlags;
};

// extraData is the last member before the tail, so only the size changes on VR. See
// ExtraDataList.h. baseForm, rotation, position, parentCell and loadedState are all confirmed
// unchanged by VR's TESObjectREFR::SetPosition (0x1402A8010), which uses [this+0x54/0x58/0x5C] for
// the position, [this+0x60] for parentCell and [this+0x68] for loadedState, then reads the cell's
// worldspace at [cell+0x120].
static_assert(sizeof(TESObjectREFR) == 0xA0 - kExtraDataListDelta);
static_assert(offsetof(TESObjectREFR, loadedState) == 0x68);
