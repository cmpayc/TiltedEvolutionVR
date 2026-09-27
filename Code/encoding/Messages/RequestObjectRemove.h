#pragma once

#include "Message.h"

#include <Structs/GameId.h>

/**
 * @brief Sent when a dropped item is taken back into an inventory, so the other clients stop showing it.
 *
 * Each client made its own world reference for that drop, and only the drop id names the same object on all of
 * them. Without this the item is correctly removed from the picker's world and left lying on the floor of
 * everybody else's.
 *
 * Static world objects need none of this: they are picked up on every client independently through activation
 * sync, which is why they already disappeared correctly.
 */
struct RequestObjectRemove final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestObjectRemove;

    RequestObjectRemove()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestObjectRemove() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestObjectRemove& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && DropId == acRhs.DropId && CellId == acRhs.CellId; }

    uint64_t DropId{};
    GameId CellId{};
};
