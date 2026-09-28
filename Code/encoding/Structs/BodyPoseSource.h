#pragma once

#include <Structs/BodyPose.h>
#include <atomic>
#include <limits>

namespace BodyTracking
{
// This counter belongs to the process, not the connection, actor or source 3D.
inline uint64_t NextSourceSequence() noexcept
{
    static std::atomic<uint64_t> sequence{};
    auto previous = sequence.load(std::memory_order_relaxed);
    do
    {
        if (previous == std::numeric_limits<uint64_t>::max()) return 0;
    } while (!sequence.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed));
    return previous + 1;
}

// Value-only handoff from the capture callback; never carries native pointers.
// Serial advances on each attempted capture, including failures. All fields in
// a snapshot must be copied under one mailbox lock by the native adapter.
enum class SourceState { TransientGap, Captured, Lost };
enum class CaptureFailure { None, Provider, TrackingUnavailable, TrackingLost, No3D, NativeRead, HmdNode, RequiredPose, GenerationExhausted, RootChanged, Exception };
enum class GripCaptureState { NotSampled, Captured, WandUnavailable, NoMatchInScan, RejectedScan, InvalidTransform, Sheathed };
struct NativeReadStats
{
    uint32_t Nodes{}, Guards{}, Regions{}, KindCalls{}, KindCacheHits{}, VirtualQueries{}, RangeCalls{}, LastRegionHits{}, NameChunks{};
    uint32_t KindHeaderChecks{}, KindNameChecks{}, KindCacheInvalidations{};
    uint32_t SkinInstances{}, EmptySkins{}, UnlinkedSkins{}, NullSkinSlots{}, MappedSkinSlots{};
    // Per-primitive counters (2026-09-23): Probes/ProbeBytes are ReadProcessMemory, VirtualQueries stays
    // VirtualQuery only (in rpm mode: only the residency fallbacks), Regions stays "VirtualQuery regions"
    // (0 in rpm mode; in vq mode the code page a Kind check resolves stays inside the backend, so the
    // count is one lower than a524de14's for the same tree), Intervals is the proven readable-interval
    // count in either mode. WriteQueries and
    // ExecutableLookups are working-set protection queries; their Fallbacks are the VirtualQuery calls made
    // for pages the working set did not hold.
    uint32_t Probes{}, ProbeBytes{}, Intervals{}, IntervalCapHits{};
    uint32_t WriteQueries{}, WriteQueryFailures{}, WriteQueryFallbacks{};
    uint32_t ExecutableLookups{}, ExecutableFallbackHits{};
};
struct SoloPreflightSchedule
{
    uint64_t LoadedAt{}, Next{};
    uint32_t Attempts{};
    bool ObserveLoad(uint64_t at)
    {
        if (!at || at == LoadedAt) return false;
        LoadedAt = Next = at; Attempts = 0;
        return true;
    }
    bool Attempt(uint64_t now)
    {
        if (!LoadedAt || now < Next || Attempts >= 10) return false;
        ++Attempts; Next = now + 2000;
        return true;
    }
};
struct SourceSample
{
    Pose Body{};
    uint64_t Serial{};
    uint64_t SteadyMs{};
    // Lost needs a positive observation (OFF, required tracking/provider or 3D
    // gone). A late callback, bad transform or failed read is only TransientGap.
    SourceState State{SourceState::TransientGap};
    std::array<uint32_t, 2> LocalGripItems{}; // value-only; mapped on update
    CaptureFailure Failure{};
    uint32_t NativeDetail{};
    uint64_t CaptureMicros{};
    std::array<GripCaptureState, 2> GripState{};
    bool Drawn{};
    NativeReadStats ReadStats{}; // all queries in the single capture reader, including HMD
    uint64_t ReadMicros{}, PoseMicros{}, GripMicros{};
    uint64_t HmdMicros{}, StableMicros{}; // inside PoseMicros: the HMD/PlayerCharacter reads, and Capture's Stable() pass
    uint32_t CaptureThread{};
};

struct SourceStream
{
    static constexpr uint64_t kFreshMs = 100;
    static constexpr uint8_t kLossSends = 3;
    uint64_t LastSendMs{};
    uint64_t LastSerial{};
    uint32_t LastGeneration{1};
    uint8_t LossSendsRemaining{};
    bool HasSent{};
    bool HadBody{};
    bool ExplicitlyLost{};

    // Transport-update context only: server time is never read in a callback.
    // The native owner may reset this object on disconnect; Sequence survives.
    bool Build(const SourceSample& aSample, uint64_t aSteadyNow, uint64_t aServerNow, Pose& aOutput) noexcept
    {
        aOutput = {};
        if (!aServerNow || (HasSent && aSteadyNow < LastSendMs)) return false;
        const bool newAttempt = aSample.Serial && aSample.Serial > LastSerial;
        const bool lossTransition = newAttempt && aSample.State == SourceState::Lost && HadBody;
        if (HasSent && aSteadyNow - LastSendMs < kSendIntervalMs && !lossTransition) return false;
        const bool timeValid = aSample.SteadyMs <= aSteadyNow && aSteadyNow - aSample.SteadyMs <= kFreshMs;
        const uint64_t age = timeValid ? aSteadyNow - aSample.SteadyMs : 0;
        Pose candidate = aSample.Body;
        candidate.Sequence = 1; // Validate before consuming the process sequence.
        candidate.CaptureTick = timeValid && aServerNow > age ? aServerNow - age : 0;
        const bool valid = aSample.State == SourceState::Captured && timeValid && candidate.HasBody() && candidate.IsValid();
        if (newAttempt) LastSerial = aSample.Serial;
        if (valid && newAttempt)
        {
            candidate.Sequence = NextSourceSequence();
            if (!candidate.Sequence) return false;
            aOutput = candidate;
            LastGeneration = candidate.Generation;
            HadBody = true;
            ExplicitlyLost = false;
            LossSendsRemaining = 0;
        }
        else
        {
            if (lossTransition)
            {
                LossSendsRemaining = kLossSends;
                HadBody = false;
                ExplicitlyLost = true;
            }
            // A single bad read or a 400 ms scheduling gap is silence, not
            // tracking loss. Already scheduled explicit-loss repeats continue.
            if (!ExplicitlyLost) return false;
            if (!LossSendsRemaining) return false;
            aOutput.Sequence = NextSourceSequence();
            if (!aOutput.Sequence) return false;
            aOutput.Generation = LastGeneration;
            aOutput.CaptureTick = aServerNow; // time of loss report; no captured body
            --LossSendsRemaining;
        }
        LastSendMs = aSteadyNow;
        HasSent = true;
        return true;
    }
};
}
