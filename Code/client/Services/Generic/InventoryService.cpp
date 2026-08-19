#include <Services/InventoryService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>
#include <Events/DynamicObjectCreatedEvent.h>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunWeaponStateUpdates();
    RunNakedNPCBugChecks();
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
    if (!serverIdRes.has_value())
    {
        spdlog::error(__FUNCTION__ ": failed to find server id, target form id: {:X}, item id: {:X}, count: {}", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
        return;
    }

    RequestInventoryChanges request;
    request.ServerId = serverIdRes.value();
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    // A drop creates a world object, and this is where it gets a name every client can agree on. Minted
    // here rather than by the server because we need it immediately, to pair it with the reference we
    // just created ourselves. The actor's server id makes it unique between players, the counter between
    // repeated drops by the same one.
    //
    // The handle is resolved here rather than in the drop hook, which runs a frame earlier while the game is
    // still finishing the reference. Doing it there left the dropper with an object that never fell.
    if (acEvent.Drop && acEvent.DroppedHandle)
    {
        TESObjectREFR* pDropped = TESObjectREFR::GetByHandle(acEvent.DroppedHandle);

        if (pDropped)
        {
            request.DropId = (static_cast<uint64_t>(request.ServerId) << 32) | ++m_nextDropId;

            m_dispatcher.trigger(DynamicObjectCreatedEvent(request.DropId, pDropped->formID));
        }
        else
        {
            spdlog::warn("Dropped object handle {:X} no longer resolves, it will not be syncable", acEvent.DroppedHandle);
        }
    }

    m_transport.Send(request);

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}, drop: {}, dropId: {:X}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId, acEvent.Drop, request.DropId);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
    if (!serverIdRes.has_value())
    {
        spdlog::error(__FUNCTION__ ": failed to find server id, actor id: {:X}, item id: {:X}, isAmmo: {}, unequip: {}, slot: {:X}", acEvent.ActorId, acEvent.ItemId, acEvent.IsAmmo, acEvent.Unequip, acEvent.EquipSlotId);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = serverIdRes.value();

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}", acEvent.ItemId, acEvent.Count, acEvent.ActorId);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.Drop)
    {
        Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
        if (!pActor)
        {
            spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ServerId);
            return;
        }

        uint32_t droppedHandle = 0;

        {
            ScopedInventoryOverride _;

            droppedHandle = pActor->DropOrPickUpObject(acMessage.Item, nullptr, nullptr);
        }

        /**
         * Our copy of the dropped object is a different reference from the dropper's, so pair the two under the
         * id that came with the drop. Without that pairing nobody can sync it once it is picked up, because
         * there is nothing about it that both clients can name.
         *
         * Resolved on the next update rather than here. The game is still building the reference at this point,
         * and resolving a handle that early is what left this path logging `as 0` and registering nothing at
         * all, which is the same fault the local drop hook hit and solved the same way.
         */
        const uint64_t cDropId = acMessage.DropId;
        const uint32_t cActorId = pActor->formID;
        const uint32_t cBaseId = acMessage.Item.BaseId.BaseId;
        const int32_t cCount = acMessage.Item.Count;

        m_world.GetRunner().Queue([this, droppedHandle, cDropId, cActorId, cBaseId, cCount]() {
            TESObjectREFR* pDropped = droppedHandle ? TESObjectREFR::GetByHandle(droppedHandle) : nullptr;

            if (pDropped && cDropId)
                m_dispatcher.trigger(DynamicObjectCreatedEvent(cDropId, pDropped->formID));

            if (pDropped)
            {
                spdlog::info("Dropped remote item {:X} (count {}) from actor {:X} as {:X}, dropId {:X}{}", cBaseId, cCount, cActorId, pDropped->formID, cDropId, cDropId ? "" : "  (not paired, the drop carried no dropId)");
                return;
            }

            /**
             * @brief Says which half of the pairing failed, because the two causes need different fixes.
             *
             * This path used to log `as 0` for both and that is where it stalled: three of four remote drops in
             * the 2026-08-19 21:17 session produced no reference, and the one that succeeded was the only one
             * whose dropId matched a locally registered one. Either the drop never produced a reference at all,
             * which means the item does not exist on this client, or it produced a handle that stopped
             * resolving within one update, which means the reference was created and then destroyed. The first
             * is a failed drop, the second is a lifetime problem, and the log could not tell them apart.
             *
             * Worth knowing while reading these: an unpaired dropped reference can never be synced afterwards,
             * because the dropId is the only name both clients share for it, and a dropped weapon with no 3D is
             * what the game crashed on twice at SkyrimVR.exe+03AD7B1.
             */
            if (!droppedHandle)
                spdlog::error("Dropped remote item {:X} (count {}) from actor {:X} produced NO REFERENCE: DropOrPickUpObject returned no handle, so this client has no copy of the item and dropId {:X} can never be paired.", cBaseId, cCount, cActorId, cDropId);
            else
                spdlog::error("Dropped remote item {:X} (count {}) from actor {:X} produced handle {:X}, which no longer resolves one update later, so dropId {:X} can never be paired.", cBaseId, cCount, cActorId, droppedHandle, cDropId);
        });
    }
    else
    {
        TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
        if (!pObject)
        {
            spdlog::warn("{}: no object for server id {:X}, item {:X} not applied", __FUNCTION__, acMessage.ServerId, acMessage.Item.BaseId.BaseId);
            return;
        }

        ScopedInventoryOverride _;

        pObject->AddOrRemoveItem(acMessage.Item);
    }
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ServerId);
        return;
    }

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    auto* pObject = Cast<TESBoundObject>(pItem);

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
        // Unequip all armor first, since the game won't auto unequip armor
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = pActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);

        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            if (pArmor)
                pEquipManager->Equip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

        /**
         * An entity outlives its actor, so this cannot be assumed to resolve.
         *
         * The form stops resolving the moment the actor is unloaded or deleted, while the entity survives
         * until the removal sweep catches up with it. Both other loops in this file check; this one did not,
         * and it crashed the second player at 22:17:51 on 2026-08-19 in RunWeaponStateUpdates, reading
         * actorState off a null pointer. actorState sits at 0xC0 minus the VR delta, which is why the fault
         * address was 0xC4. The entity it tripped on was the Fox 105A25, whose actor had been removed
         * moments earlier.
         */
        if (!pActor)
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}

void InventoryService::RunNakedNPCBugChecks() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        if (pActor->GetExtension()->IsPlayer())
            continue;

        if (pActor->IsDead())
            continue;

        if (pActor->IsWearingBodyPiece())
            continue;

        if (!pActor->ShouldWearBodyPiece())
            continue;

        // Don't broadcast changes, it'll just make things messier.
        // If all clients have this problem, they'll all fix it individually.
        ScopedEquipOverride seo;
        ScopedInventoryOverride sio;

        pActor->ResetInventory(false);
    }
}
