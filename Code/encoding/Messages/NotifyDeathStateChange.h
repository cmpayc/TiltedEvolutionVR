#pragma once

#include "Message.h"

struct NotifyDeathStateChange final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDeathStateChange;

    NotifyDeathStateChange()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

#if TP_SKYRIMVR
    bool operator==(const NotifyDeathStateChange& acRhs) const noexcept { return Id == acRhs.Id && OwnershipEpoch == acRhs.OwnershipEpoch && IsDead == acRhs.IsDead && IsBleedingOut == acRhs.IsBleedingOut && GetOpcode() == acRhs.GetOpcode(); }
#else
    bool operator==(const NotifyDeathStateChange& acRhs) const noexcept { return Id == acRhs.Id && OwnershipEpoch == acRhs.OwnershipEpoch && IsDead == acRhs.IsDead && GetOpcode() == acRhs.GetOpcode(); }
#endif

    uint32_t Id;
    uint32_t OwnershipEpoch{};
    bool IsDead;

    // Whether the actor is down but not dead. See RequestDeathStateChange::IsBleedingOut for why IsDead alone
    // cannot describe an essential actor and what happens to a client left to work it out from health.
    bool IsBleedingOut{};
};
