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

    bool operator==(const NotifyDeathStateChange& acRhs) const noexcept { return Id == acRhs.Id && IsDead == acRhs.IsDead && IsBleedingOut == acRhs.IsBleedingOut && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id;
    bool IsDead;

    // Whether the actor is down but not dead. See RequestDeathStateChange::IsBleedingOut for why IsDead alone
    // cannot describe an essential actor and what happens to a client left to work it out from health.
    bool IsBleedingOut{};
};
