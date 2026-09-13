#include <Services/CalendarService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/RequestSleepTime.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Interface/UI.h>
#include <PlayerCharacter.h>
#include <TimeManager.h>

#include <Services/PartyService.h>
#include <Services/TransportService.h>

constexpr float kTransitionSpeed = 5.f;

bool CalendarService::s_gameClockLocked = false;

bool CalendarService::AllowGameTick() noexcept
{
    return !s_gameClockLocked;
}

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld), m_transport(aTransport)
{
    m_timeUpdateConnection = aDispatcher.sink<ServerTimeSettings>().connect<&CalendarService::OnTimeUpdate>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::HandleUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CalendarService::OnDisconnected>(this);
}

void CalendarService::OnTimeUpdate(const ServerTimeSettings& acMessage) noexcept
{
    // disable the game clock
    ToggleGameClock(false);
    m_onlineTime.m_timeModel.TimeScale = acMessage.timeModel.TimeScale;
    m_onlineTime.m_timeModel.Time = acMessage.timeModel.Time;

    if (m_world.GetServerSettings().SyncPlayerCalendar)
    {
        m_onlineTime.m_timeModel.Day = acMessage.timeModel.Day;
        m_onlineTime.m_timeModel.Month = acMessage.timeModel.Month;
        m_onlineTime.m_timeModel.Year = acMessage.timeModel.Year;
    }
    else
    {
        m_onlineTime.m_timeModel.Day = m_offlineTime.m_timeModel.Day;
        m_onlineTime.m_timeModel.Month = m_offlineTime.m_timeModel.Month;
        m_onlineTime.m_timeModel.Year = m_offlineTime.m_timeModel.Year;
    }
}

void CalendarService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // signal a time transition
    m_fadeTimer = 0.f;
    ToggleGameClock(true);
}

float CalendarService::TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const
{
    const auto t = aTo.Time - aFrom.Time;
    if (t < 0.f)
    {
        const auto v = t + 24.f;
        // interpolate on the time difference, not the time
        const auto x = TiltedPhoques::Lerp(0.f, v, m_fadeTimer / kTransitionSpeed) + aFrom.Time;

        return TiltedPhoques::Mod(x, 24.f);
    }

    return TiltedPhoques::Lerp(aFrom.Time, aTo.Time, m_fadeTimer / kTransitionSpeed);
}

void CalendarService::ToggleGameClock(bool aEnable)
{
    auto* pGameTime = TimeData::Get();
    m_offlineTime.m_timeModel.Day = pGameTime->GameDay->f;
    m_offlineTime.m_timeModel.Month = pGameTime->GameMonth->f;
    m_offlineTime.m_timeModel.Year = pGameTime->GameYear->f;
    m_offlineTime.m_timeModel.Time = pGameTime->GameHour->f;
    m_offlineTime.m_timeModel.TimeScale = pGameTime->TimeScale->f;

    s_gameClockLocked = !aEnable;
}

bool CalendarService::IsSleepMenuOpen() noexcept
{
    UI* pUI = UI::Get();
    if (!pUI)
        return false;

    // Both sleeping and waiting run through this one menu, and both should move the clock.
    static const BSFixedString cMenuName("Sleep/Wait Menu");

    return pUI->GetMenuOpen(cMenuName);
}

TimeModel CalendarService::ReadGameClock() noexcept
{
    auto* pGameTime = TimeData::Get();

    TimeModel model{};
    model.TimeScale = pGameTime->TimeScale->f;
    model.Time = pGameTime->GameHour->f;
    model.Day = static_cast<uint32_t>(pGameTime->GameDay->f);
    model.Month = static_cast<uint32_t>(pGameTime->GameMonth->f);
    model.Year = static_cast<uint32_t>(pGameTime->GameYear->f);

    return model;
}

void CalendarService::UpdateSleep(const double aDelta) noexcept
{
    // How long the clock stays free after the menu closes. The fast forward runs after that point, not while
    // the menu is up, so releasing it only for as long as the menu is open would catch nothing at all.
    constexpr double kFastForwardGrace = 3.0;
    // Below this much game time the sleep is treated as having been cancelled. An hour of sleep is 1.0, so
    // this is a minute, comfortably below anything the menu can be asked for and above the drift the clock
    // picks up from running freely for the few seconds it is released.
    constexpr float kMovedEnough = 1.f / 60.f;

    if (!m_transport.IsOnline())
        return;

    if (IsSleepMenuOpen())
    {
        if (!m_sleeping)
        {
            // The server decides whether sleeping does anything at all. Checked here rather than only on
            // arrival, because the alternative is to fast forward this client's clock and have the server's
            // next resync drag it back, which is worse than no time passing.
            if (!m_world.GetServerSettings().SleepEnabled)
            {
                spdlog::info("Sleep: this server has bEnableSleep off, so no time will pass");

                m_sleeping = true;
                m_sleepGrace = 0.0;
                m_beforeSleep = TimeModel{};

                return;
            }

            if (!m_world.GetPartyService().IsLeader())
            {
                // Once per menu open, and it is the answer to "why did sleeping do nothing". Not a warning:
                // this is the configured behaviour, not a fault.
                spdlog::info("Sleep: only the party leader can move the shared clock, so no time will pass");

                m_sleeping = true;
                m_sleepGrace = 0.0;
                m_beforeSleep = TimeModel{};

                return;
            }

            m_sleeping = true;
            m_beforeSleep = ReadGameClock();

            spdlog::info("Sleep: the clock is going back to the game at {:.2f} on day {} of month {}, year {}", m_beforeSleep.Time, m_beforeSleep.Day, m_beforeSleep.Month, m_beforeSleep.Year);

            ToggleGameClock(true);
        }

        m_sleepGrace = kFastForwardGrace;

        return;
    }

    if (!m_sleeping)
        return;

    // The menu has closed. Give the game a moment to actually move the clock before reading it.
    m_sleepGrace -= aDelta;
    if (m_sleepGrace > 0.0)
        return;

    m_sleeping = false;

    // A non leader never released the clock, so there is nothing to take back.
    if (m_beforeSleep == TimeModel{})
        return;

    const TimeModel cAfter = ReadGameClock();

    const float cMoved = DateTime(cAfter).GetTimeInDays() - DateTime(m_beforeSleep).GetTimeInDays();

    // Adopt whatever the game arrived at, so that locking the clock again does not snap the player back to
    // where they lay down. The server's own resync follows and will agree, or will correct us if it does not.
    m_onlineTime.m_timeModel.Time = cAfter.Time;
    m_onlineTime.m_timeModel.Day = cAfter.Day;
    m_onlineTime.m_timeModel.Month = cAfter.Month;
    m_onlineTime.m_timeModel.Year = cAfter.Year;
    m_lastTick = m_world.GetTick();

    ToggleGameClock(false);

    if (cMoved * 24.f < kMovedEnough)
    {
        spdlog::info("Sleep: the clock came back unmoved, so the menu was opened and cancelled");

        return;
    }

    spdlog::info("Sleep: {:.2f} hours passed, so the server is being asked to move everybody to {:.2f} on day {} of month {}, year {}", cMoved * 24.f, cAfter.Time, cAfter.Day, cAfter.Month, cAfter.Year);

    RequestSleepTime request{};
    request.timeModel = cAfter;

    m_transport.Send(request);
}

void CalendarService::HandleUpdate(const UpdateEvent& aEvent) noexcept
{
    UpdateSleep(aEvent.Delta);

    if (s_gameClockLocked)
    {
        const auto updateDelta = static_cast<float>(aEvent.Delta);
        auto* pGameTime = TimeData::Get();

        if (!m_lastTick)
            m_lastTick = m_world.GetTick();

        const auto now = m_world.GetTick();

        // we got disconnected or the client got ahead of us
        if (now < m_lastTick)
            return;

        const auto delta = now - m_lastTick;
        m_lastTick = now;

        m_onlineTime.Update(delta);
        pGameTime->TimeScale->f = m_onlineTime.m_timeModel.TimeScale;
        pGameTime->GameDay->f = m_onlineTime.m_timeModel.Day;
        pGameTime->GameMonth->f = m_onlineTime.m_timeModel.Month;
        pGameTime->GameYear->f = m_onlineTime.m_timeModel.Year;
        pGameTime->GameDaysPassed->f += m_onlineTime.GetDeltaTime(delta) * (1.f / 24.f);

        // time transition in
        if (m_fadeTimer < kTransitionSpeed)
        {
            pGameTime->GameHour->f = TimeInterpolate(m_offlineTime.m_timeModel, m_onlineTime.m_timeModel);
            m_fadeTimer += updateDelta;
        }
        else
            pGameTime->GameHour->f = m_onlineTime.m_timeModel.Time;
    }
}
