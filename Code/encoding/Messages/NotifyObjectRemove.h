#pragma once

#include "Message.h"

/**
 * @brief Tells a client to stop showing a dropped item, because somebody picked it up.
 *
 * No CellId: the server has already filtered on it. See RequestObjectRemove.
 */
struct NotifyObjectRemove final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyObjectRemove;

    NotifyObjectRemove()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyObjectRemove() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyObjectRemove& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && DropId == acRhs.DropId; }

    uint64_t DropId{};
};
