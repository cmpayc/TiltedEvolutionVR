#pragma once

#include <Structs/BodyPose.h>
#include <algorithm>

namespace BodyTracking
{
enum class Selection
{
    Ready,
    Empty,
    Warming,
    Stale,
    ClockOutsideWindow,
    SourceLost
};

// Owned by one remote actor/3D lifetime. Its owner must clear it on removal,
// disconnect and receiver content replacement. No native pointers live here.
struct History
{
    static constexpr size_t kCapacity = 32;
    struct Sample
    {
        uint64_t ServerTick{};
        Pose Body{};
    };
    std::array<Sample, kCapacity> Samples{};
    size_t Count{};
    uint64_t LastSequence{};
    uint64_t LastServerTick{};
    uint32_t Generation{};

    bool Push(const Pose& aPose, uint64_t aServerTick) noexcept
    {
        if (!aPose.IsValid() || !aServerTick || aPose.Sequence <= LastSequence || aServerTick < LastServerTick)
            return false;
        LastSequence = aPose.Sequence;
        LastServerTick = aServerTick;
        if (Generation != aPose.Generation) Count = 0;
        Generation = aPose.Generation;
        // A loss is a timeline event, not an instruction to throw away the
        // preceding valid segment. Never interpolate a body across this event.
        if (Count == kCapacity)
        {
            std::move(Samples.begin() + 1, Samples.end(), Samples.begin());
            --Count;
        }
        Samples[Count++] = {aServerTick, aPose};
        return true;
    }

    Selection Select(uint64_t aNow, Pose& aOutput) const noexcept
    {
        aOutput = {};
        if (!Count) return Selection::Empty;
        const auto& latest = Samples[Count - 1];
        if (latest.Body.CaptureTick > aNow && latest.Body.CaptureTick - aNow > kMaxAgeMs)
            return Selection::ClockOutsideWindow;
        if (aNow > latest.Body.CaptureTick && aNow - latest.Body.CaptureTick > kMaxAgeMs)
            return Selection::Stale;
        if (aNow < kPresentationDelayMs) return Selection::Warming;
        const auto target = aNow - kPresentationDelayMs;
        if (target < Samples[0].ServerTick) return Selection::Warming;
        size_t lower = Count - 1;
        for (size_t i = 1; i < Count; ++i)
            if (Samples[i].ServerTick > target) { lower = i - 1; break; }
        const auto& first = Samples[lower];
        if (!first.Body.HasBody()) return Selection::SourceLost;
        if (aNow > first.Body.CaptureTick && aNow - first.Body.CaptureTick > kMaxAgeMs)
            return Selection::Stale;
        aOutput = first.Body;
        if (lower + 1 == Count) return Selection::Ready;
        const auto& second = Samples[lower + 1];
        if (!second.Body.HasBody()) return Selection::Ready;
        // Selection brackets first <= target < second, so equal accepted ticks
        // cannot form this pair. Defensive against accidentally damaged state.
        if (second.ServerTick <= first.ServerTick) return Selection::Ready;
        const float weight = static_cast<float>(target - first.ServerTick) / static_cast<float>(second.ServerTick - first.ServerTick);
        aOutput.Mask &= second.Body.Mask;
        aOutput.GripMask &= second.Body.GripMask;
        for (size_t i = 0; i < aOutput.Grips.size(); ++i)
            if (first.Body.GripItems[i] != second.Body.GripItems[i]) aOutput.GripMask &= ~(1u << i);
        const auto blend = [weight](const BonePose& a, const BonePose& b)
        {
            BonePose result{};
            result.Position = glm::mix(a.Position, b.Position, weight);
            result.Rotation = glm::normalize(glm::slerp(static_cast<const glm::quat&>(a.Rotation), static_cast<const glm::quat&>(b.Rotation), weight));
            return result;
        };
        for (size_t i = 0; i < aOutput.Bones.size(); ++i)
            if (aOutput.Mask & (1u << i)) aOutput.Bones[i] = blend(first.Body.Bones[i], second.Body.Bones[i]);
        for (size_t i = 0; i < aOutput.Grips.size(); ++i)
            if (aOutput.GripMask & (1u << i)) aOutput.Grips[i] = blend(first.Body.Grips[i], second.Body.Grips[i]);
        return Selection::Ready;
    }
};
}
