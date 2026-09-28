#pragma once

#include <Structs/BodyPose.h>

namespace BodyTracking
{
enum class Admission { Accepted, Order, Rate, Clock };

// Called only after actor ownership, schema and source clock validation.
// Registry entity lifetime owns this state; Generation does not reset ordering.
struct RelayState
{
    uint64_t Sequence{};
    uint64_t LastTick{};
    bool HasRelayed{};
    bool HadBody{};

    Admission Admit(const Pose& aPose, uint64_t aTick) noexcept
    {
        if (aPose.Sequence <= Sequence) return Admission::Order;
        if (HasRelayed && aTick < LastTick) return Admission::Clock;
        // Remember valid attempts even when rate limited: an older packet must
        // not resurrect a sample that preceded an already observed loss.
        Sequence = aPose.Sequence;
        const bool lossTransition = HadBody && !aPose.HasBody();
        // One immediate stop bypasses the gate. Duplicate stops remain bounded;
        // the producer repeats them at its ordinary 33 ms cadence.
        if (HasRelayed && aTick - LastTick < 20 && !lossTransition) return Admission::Rate;
        LastTick = aTick;
        HasRelayed = true;
        HadBody = aPose.HasBody();
        return Admission::Accepted;
    }
};

enum class RelayVerdict { Relay, Legacy, NotOwner, Malformed, Clock, Order, Rate };

/**
 * @brief The server's whole decision for one body pose request, in order.
 *
 * With the server's bUseLegacyHandPose on, nothing is relayed whatever the client sends. aGetState is called
 * only once the request is known to come from the owner of a valid, fresh pose, because the relay state lives on
 * the sender's entity and must not be created for a request that failed those checks.
 */
template <class TGetState>
RelayVerdict DecideRelay(const bool aLegacyHandPose, const bool aOwnedBySender, const Pose& acPose, const uint64_t aTick, TGetState&& aGetState) noexcept
{
    if (aLegacyHandPose)
        return RelayVerdict::Legacy;
    if (!aOwnedBySender)
        return RelayVerdict::NotOwner;
    if (!acPose.IsValid())
        return RelayVerdict::Malformed;

    const uint64_t cSource = acPose.CaptureTick;
    if ((aTick > cSource && aTick - cSource > kMaxAgeMs) || (cSource > aTick && cSource - aTick > kMaxAgeMs))
        return RelayVerdict::Clock;

    switch (aGetState().Admit(acPose, aTick))
    {
    case Admission::Order: return RelayVerdict::Order;
    case Admission::Rate: return RelayVerdict::Rate;
    case Admission::Clock: return RelayVerdict::Clock;
    case Admission::Accepted: break;
    }
    return RelayVerdict::Relay;
}
}
