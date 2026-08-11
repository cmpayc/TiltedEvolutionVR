#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

/**
 * @brief Relays a held object's transform to the other clients in the cell.
 *
 * No CellId, unlike the request: the server has already filtered on it by the time this is sent.
 */
struct NotifyObjectTransform final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyObjectTransform;

    NotifyObjectTransform()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyObjectTransform() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyObjectTransform& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Position == acRhs.Position && Rotation == acRhs.Rotation && IsReleased == acRhs.IsReleased;
    }

    GameId Id{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};

    // Stop driving the object and let local physics have it back.
    bool IsReleased{};
};
