#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/PlayerLeaveCellEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#if TP_SKYRIMVR
#include <Messages/RequestObjectTransform.h>
#include <Messages/NotifyObjectTransform.h>
#include <Messages/RequestObjectRemove.h>
#include <Messages/NotifyObjectRemove.h>
#endif

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
#if TP_SKYRIMVR
    m_objectTransformConnection = aDispatcher.sink<PacketEvent<RequestObjectTransform>>().connect<&ObjectService::OnObjectTransform>(this);
    m_objectRemoveConnection = aDispatcher.sink<PacketEvent<RequestObjectRemove>>().connect<&ObjectService::OnObjectRemove>(this);
#endif
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetCellComponent().Cell == acEvent.OldCell)
            return;
    }

    auto objectView = m_world.view<ObjectComponent, CellIdComponent>();
    Vector<entt::entity> toDestroy;

    for (auto entity : objectView)
    {
        const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);

        if (cellIdComponent.Cell != acEvent.OldCell)
            continue;

        toDestroy.push_back(entity);
    }

    for (auto& entity : toDestroy)
    {
        m_world.destroy(entity);
    }
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;

            objectData.IsSenderFirst = false;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.pPlayer);
            objectComponent.CurrentLockData = object.CurrentLockData;

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = object.CurrentInventory;

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = true;

            response.Objects.push_back(objectData);
        }
    }

    if (!response.Objects.empty())
        acMessage.pPlayer->Send(response);
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
        {
            pPlayer->Send(notifyActivate);
        }
    }
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        if (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
            pPlayer->Send(notifyLockChange);
    }
}

#if TP_SKYRIMVR
// Relayed straight through on receipt, like OnActivate above, rather than batched onto a tick. A held
// object is attached to somebody's hand, so every extra frame of delay is visible, and the sender only
// emits these while something is actually being held.
//
// Nothing is stored server side. A held object needs no authority record: the transform is a live
// stream keyed on a form id that already means the same thing on every client, and it stops on its own
// when the hand lets go.
void ObjectService::OnObjectTransform(const PacketEvent<RequestObjectTransform>& acMessage) const noexcept
{
    const auto& packet = acMessage.Packet;

    NotifyObjectTransform notify{};

    // Both identifiers have to come across, and only one of them is ever set. A static reference travels as
    // Id, a dropped item as DropId, so forgetting either leaves the receiver with nothing to resolve and no
    // way to tell that from a message it never got.
    notify.Id = packet.Id;
    notify.DropId = packet.DropId;
    notify.Position = packet.Position;
    notify.Rotation = packet.Rotation;
    notify.IsReleased = packet.IsReleased;

    size_t sent = 0;
    size_t others = 0;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        ++others;

        if (pPlayer->GetCellComponent().Cell == packet.CellId)
        {
            pPlayer->Send(notify);
            ++sent;
        }
    }

    // If a client is holding something and nobody else is being sent it, the cell filter is rejecting
    // everyone and no amount of looking at the two client logs will show why, because neither can see this
    // decision. Counted rather than timed, since these arrive at up to 30 Hz per held object.
    if (others > 0 && sent == 0)
    {
        static uint32_t missCount = 0;

        if ((missCount++ % 60) == 0)
            spdlog::warn("Object transform relayed to nobody: {} other players, none in cell {:X}:{:X}", others, packet.CellId.ModId, packet.CellId.BaseId);
    }
}

// A dropped item was taken into somebody's inventory, so everyone else has a copy to delete. Relayed on receipt
// like the transform, and nothing is stored: the drop id is the clients' own name for the object and the server
// has no opinion about it.
void ObjectService::OnObjectRemove(const PacketEvent<RequestObjectRemove>& acMessage) const noexcept
{
    const auto& packet = acMessage.Packet;

    NotifyObjectRemove notify{};
    notify.DropId = packet.DropId;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        if (pPlayer->GetCellComponent().Cell == packet.CellId)
            pPlayer->Send(notify);
    }
}
#endif

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    auto& packet = acMessage.Packet;

    NotifyScriptAnimation message{};
    message.FormID = packet.FormID;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        pPlayer->Send(message);
    }
}
