#include <Services/CommandService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/SetTimeCommandRequest.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>
#include <Messages/RequestSleepTime.h>
#include <Messages/ServerTimeSettings.h>

#include <Setting.h>

namespace
{
Console::Setting bAnnounceServer{"LiveServices:bAnnounceServer", "Whether to list the server on the public server list", false};
Console::Setting bEnableSleep{"Gameplay:bEnableSleep", "Let sleeping and waiting move the shared clock forward for everybody. Only the party leader's sleep counts, and only on a private server, the same rule the settime command follows", true};
}

CommandService::CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_setTimeConnection = aDispatcher.sink<PacketEvent<SetTimeCommandRequest>>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportCommandRequest>>().connect<&CommandService::OnTeleportCommandRequest>(this);
    m_sleepTimeConnection = aDispatcher.sink<PacketEvent<RequestSleepTime>>().connect<&CommandService::OnSleepTimeRequest>(this);
}

bool CommandService::CanCommandTime(const Player* apPlayer, const uint32_t aPlayerId) const noexcept
{
    // Admin override: always allowed
    for (const auto session : GameServer::Get()->GetAdminSessions())
    {
        if (PlayerManager::Get()->GetByConnectionId(session)->GetId() == aPlayerId)
            return true;
    }

    // Party leader allowed on private servers only
    return m_world.GetPartyService().IsPlayerLeader(apPlayer) && !bAnnounceServer;
}

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    NotifySetTimeResult response{};

    if (!CanCommandTime(acMessage.pPlayer, static_cast<uint32_t>(acMessage.Packet.PlayerId)))
    {
        response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
        acMessage.pPlayer->Send(response);

        return;
    }

    const auto cHours = static_cast<int>(acMessage.Packet.Hours);
    const auto cMinutes = static_cast<int>(acMessage.Packet.Minutes);

    m_world.GetCalendarService().SetTime(cHours, cMinutes, m_world.GetCalendarService().GetTimeScale());

    response.Result = NotifySetTimeResult::SetTimeResult::kSuccess;
    acMessage.pPlayer->Send(response);
}

void CommandService::OnSleepTimeRequest(const PacketEvent<RequestSleepTime>& acMessage) const noexcept
{
    const TimeModel& cWanted = acMessage.Packet.timeModel;

    // No reply message. Nothing in the client is waiting on one: it has already moved its own clock, and the
    // resync every player gets on success is what tells it whether the server agreed.
    //
    // The setting is checked here as well as on the client, which already refuses to release its clock when
    // it is off. A client is not something to take on trust, and this is the write that would move everybody.
    if (!bEnableSleep || !CanCommandTime(acMessage.pPlayer, acMessage.pPlayer->GetId()))
    {
        spdlog::info("Player {:X} slept to {:.2f} on day {}, but may not move the shared clock, so it stays where it was", acMessage.pPlayer->GetId(), cWanted.Time, cWanted.Day);

        // Puts their own clock back, since they have already fast forwarded it locally.
        ServerTimeSettings resync{};
        resync.timeModel = m_world.GetCalendarService().GetTimeModel();

        acMessage.pPlayer->Send(resync);

        return;
    }

    if (!m_world.GetCalendarService().SetTimeAndDate(cWanted))
    {
        spdlog::warn("Player {:X} asked to sleep to {:.2f} on day {} of month {}, year {}, which the calendar refused", acMessage.pPlayer->GetId(), cWanted.Time, cWanted.Day, cWanted.Month, cWanted.Year);

        return;
    }

    spdlog::info("Player {:X} slept, so the shared clock is now {:.2f} on day {} of month {}, year {}", acMessage.pPlayer->GetId(), cWanted.Time, cWanted.Day, cWanted.Month, cWanted.Year);
}

void CommandService::OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = nullptr;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetUsername() == acMessage.Packet.TargetPlayer)
            pTargetPlayer = pPlayer;
    }

    TeleportCommandResponse response{};
    if (pTargetPlayer)
    {
        auto character = pTargetPlayer->GetCharacter();
        if (character)
        {
            const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
            if (pMovementComponent)
            {
                const auto& cellComponent = pTargetPlayer->GetCellComponent();
                response.CellId = cellComponent.Cell;
                response.Position = pMovementComponent->Position;
                response.WorldSpaceId = cellComponent.WorldSpaceId;
            }
        }
    }

    acMessage.pPlayer->Send(response);
}
