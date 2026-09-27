#pragma once

#include <Events/PacketEvent.h>
#include <DateTime.h>
#include <Structs/GameId.h>

struct World;
struct UpdateEvent;
struct PlayerJoinEvent;

/**
 * @brief Manages time and date of the world.
 */
class CalendarService
{
public:
    CalendarService(World& aWorld, entt::dispatcher& aDispatcher);

    // we use these types for SOL
    // this is done this way because SOL
    // provides direct support for these
    using TTime = std::pair<int, int>;
    using TDate = std::tuple<int, int, int>;

    bool SetTime(int aHour, int aMinutes, float aScale) noexcept;
    bool SetDate(int aDay, int aMonth, float aYear) noexcept;

    /**
     * @brief Moves the clock to an hour and a date at once, for a client that has just slept through both.
     *
     * Separate from SetTime and SetDate because sleeping crosses midnight, and calling those two in turn would
     * put out two resyncs, the first of them describing a world that never existed: the new hour on the old
     * day. TimeScale is deliberately not taken from the caller, since the server's configuration owns it.
     *
     * Refuses to move the clock backwards. Sleeping only ever goes forwards, so a request that does not is a
     * client whose clock has drifted or is lying, and adopting it would drag everybody else back with it.
     */
    bool SetTimeAndDate(const TimeModel& acModel) noexcept;

    // returns hours, minutes
    TTime GetTime() const noexcept;
    static TTime GetRealTime() noexcept;

    // returns dd/mm/yy
    TDate GetDate() const noexcept;

    float GetTimeScale() const noexcept { return m_dateTime.m_timeModel.TimeScale; }

    // The clock as it stands, exactly. GetTime and GetDate exist for the scripting bindings and round through
    // hours and minutes on the way out, which is lossy.
    const TimeModel& GetTimeModel() const noexcept { return m_dateTime.m_timeModel; }
    bool SetTimeScale(float aScale) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void SendTimeResync() noexcept;

    DateTime m_dateTime;
    uint64_t m_lastTick = 0;
    bool m_timeSetFromFirstPlayer = false;

    World& m_world;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
