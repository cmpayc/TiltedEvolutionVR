#pragma once

#include <cstdint>

/**
 * Which pose sync a VR client runs, decided from the server's bUseLegacyHandPose.
 *
 * Body (the default): every VR player's tracked full body is captured, sent and shown on the other clients, and
 * the hand pose sync only covers a puppet the body lane is not writing. Legacy: the original hand pose sync alone,
 * with hands not synced while a weapon is drawn and nothing body related captured, sent or shown. Offline: nothing
 * is sent or written, whatever the last server said.
 *
 * Pure, so both services and the tests read the same answer. F10 is separate: it only stops this client showing
 * other players' tracked poses and applies in either mode.
 */
namespace PoseSyncMode
{
enum class Mode : uint8_t { Offline, Body, Legacy };

inline Mode Decide(const bool aConnected, const bool aUseLegacyHandPose) noexcept
{
    if (!aConnected)
        return Mode::Offline;
    return aUseLegacyHandPose ? Mode::Legacy : Mode::Body;
}

// The body lane captures, sends, receives and writes.
inline bool BodyLane(const Mode aMode) noexcept { return aMode == Mode::Body; }

// The sender keeps sending its hands while a weapon is drawn. Legacy keeps the original rule: the game's
// animation owns the arms while drawn.
inline bool SyncHandsWhileDrawn(const Mode aMode) noexcept { return aMode == Mode::Body; }
}
