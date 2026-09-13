#pragma once

#include "Message.h"
#include <Structs/Inventory.h>

struct RequestInventoryChanges final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestInventoryChanges;

    RequestInventoryChanges()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestInventoryChanges() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestInventoryChanges& acRhs) const noexcept
    {
#if TP_SKYRIMVR
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Item == acRhs.Item && Drop == acRhs.Drop && UpdateClients == acRhs.UpdateClients && DropId == acRhs.DropId;
#else
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Item == acRhs.Item && Drop == acRhs.Drop && UpdateClients == acRhs.UpdateClients;
#endif
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    Inventory::Entry Item{};
    bool Drop = false;
    bool UpdateClients = true;

#if TP_SKYRIMVR
    /**
     * @brief Names this particular drop, so every client can agree on the object it creates. Zero when
     * this is not a drop.
     *
     * Minted by the dropping client rather than the server, because the dropper needs it as soon as it
     * creates its own copy of the object and the server has nothing to add. Uniqueness comes from
     * combining the dropping actor's server id with a per-client counter, so no coordination is required.
     */
    uint64_t DropId{};
#endif
};
