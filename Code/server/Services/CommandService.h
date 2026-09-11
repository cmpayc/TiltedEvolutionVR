#pragma once

#include <Events/PacketEvent.h>

struct World;
struct Player;
struct TeleportCommandRequest;
struct SetTimeCommandRequest;
struct RequestSleepTime;

/**
 * @brief Processes incoming commands.
 */
struct CommandService
{
    CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CommandService() noexcept = default;

    TP_NOCOPYMOVE(CommandService);

protected:
    void OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept;
    /**
     * @brief A client has slept, and wants the shared clock moved to where its own now is.
     *
     * Held to the same permission as the /settime command, which is what the sleep is: one player deciding
     * what hour everybody else is in. See CanCommandTime.
     */
    void OnSleepTimeRequest(const PacketEvent<RequestSleepTime>& acMessage) const noexcept;

    /**
     * @brief Whether this player is allowed to move the shared clock.
     *
     * An admin always is. A party leader is, but only on a private server, since on a listed one the leader is
     * whoever happened to arrive first and the clock belongs to everybody.
     */
    bool CanCommandTime(const Player* apPlayer, uint32_t aPlayerId) const noexcept;
    /**
     * @brief Returns the location of the target player of the teleport command.
     */
    void OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept;

private:
    World& m_world;

    entt::scoped_connection m_setTimeConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_sleepTimeConnection;
};
