#pragma once

#include <cstdint>

/**
 * Who poses a remote player's puppet this frame, decided once per remote per render frame.
 *
 * The body stream owns a puppet on every frame it writes. When it misses a frame — reference rebuild at join,
 * hands returning after an idle, F10 OFF→ON — the legacy hand-pose path used to step in for that frame and the
 * puppet flashed a legacy arm pose (2026-09-23 paired run: every legacy arm write sat at exactly those moments
 * on both seats). Two bounded holds keep the legacy path off:
 *   - after a real body write, for kBodyLatchMs (a body stream that truly stops hands the puppet back after
 *     that, instead of after zero);
 *   - when the body's history is Ready but it has not written yet (first acquire), for kBodyLatchMs from the
 *     first Ready frame; a body path that then still cannot write (reference building, retry backoff, refusal
 *     streak) hands the puppet to the legacy path exactly as before this build.
 */
namespace HandPoseLatch
{
constexpr uint64_t kBodyLatchMs = 250;

enum class Owner : uint8_t { Body, Latched, Legacy };

struct State
{
    uint64_t OwnedAtMs{0};   // steady ms of the body's last successful write to this puppet
    uint64_t ClaimedAtMs{0}; // steady ms of the first Ready frame without a write, 0 when not claiming
};

inline Owner Decide(const bool aWrittenNow, const bool aReadyNow, State& aState, const uint64_t aNowMs, const uint64_t aLatchMs = kBodyLatchMs) noexcept
{
    if (aWrittenNow)
    {
        aState.OwnedAtMs = aNowMs;
        aState.ClaimedAtMs = 0;
        return Owner::Body;
    }
    if (aState.OwnedAtMs && aNowMs >= aState.OwnedAtMs && aNowMs - aState.OwnedAtMs < aLatchMs)
        return Owner::Latched;
    if (aReadyNow)
    {
        if (!aState.ClaimedAtMs) aState.ClaimedAtMs = aNowMs;
        if (aNowMs >= aState.ClaimedAtMs && aNowMs - aState.ClaimedAtMs < aLatchMs)
            return Owner::Latched;
        return Owner::Legacy;
    }
    aState.ClaimedAtMs = 0;
    return Owner::Legacy;
}

// The idle rule in body mode: head-only posing is skipped whenever the hands are absent, drawn or not, because a
// tracked head on an otherwise animated idle body reads worse than the animation alone. Legacy mode keeps the
// original behaviour and poses the head whenever it has one, hands or not.
inline bool HeadOnlyIdle(const bool aHasHands, const bool aBodyMode) noexcept
{
    return aBodyMode && !aHasHands;
}
}
