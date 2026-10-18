#pragma once

#include <DateTime.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct UpdateEvent;
struct TransportService;

/**
 * @brief Handles time sync.
 */
class CalendarService final : public BSTEventSink<TESActivateEvent>
{
public:
    CalendarService(World&, entt::dispatcher&, TransportService&);

    static bool AllowGameTick() noexcept;

private:
    void OnTimeUpdate(const ServerTimeSettings&) noexcept;
    void HandleUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;

    void ToggleGameClock(bool aEnable);
    float TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const;

    /**
     * @brief Lets the game fast forward its own clock while the player sleeps or waits, then publishes it.
     *
     * Sleeping does nothing at all while a server owns the clock, and it is blocked twice over: the game's
     * time simulation is switched off in HookSimulateTime, and HandleUpdate below rewrites the hour from the
     * server's model on every frame, so anything that did move it would be undone within a frame.
     *
     * Rather than work out how many hours were asked for and apply them ourselves, this hands the clock back
     * to the game for the length of the sleep and reads the answer off it afterwards. The game's own sleep
     * logic then does what it always does, whichever way it does it, and the result is what gets sent.
     *
     * Only the party leader's sleep moves the shared clock, so only the party leader's clock is released.
     * Anybody else keeps the behaviour they have today, which is that no time passes, and the alternative is
     * worse: their clock would jump forward and be snapped back by the next resync.
     */
    void UpdateSleep(double aDelta) noexcept;

    // The game's clock as it stands. Not the server model, which is what HandleUpdate writes into it.
    static TimeModel ReadGameClock() noexcept;

    static bool IsSleepMenuOpen() noexcept;

    entt::scoped_connection m_timeUpdateConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;

    DateTime m_onlineTime;
    DateTime m_offlineTime;
    float m_fadeTimer = 0.f;
    static bool s_gameClockLocked;

    uint64_t m_lastTick = 0;
    uint64_t m_lastLogTick = 0;

    // Set while the clock has been handed back to the game for a sleep, with the clock as it was at the
    // moment it was handed over, and the time left to wait for the fast forward to finish after the menu
    // closes. See UpdateSleep.
    bool m_sleeping = false;
    double m_sleepGrace = 0.0;
    TimeModel m_beforeSleep{};
    World& m_world;
    TransportService& m_transport;
};
