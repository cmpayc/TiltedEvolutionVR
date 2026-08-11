#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

/**
 * @brief Sent while a VR hand holds a world object, and once more when it lets go.
 *
 * Keyed on GameId rather than a server entity id, the way ActivateRequest and LockChangeRequest are.
 * A static world reference already resolves to the same object on every client through ModSystem, so
 * no registration or id handshake is needed. Only static references are ever sent: a temporary one
 * (form id at or above 0xFF000000, anything created at runtime) names nothing on another client.
 */
struct RequestObjectTransform final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestObjectTransform;

    RequestObjectTransform()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestObjectTransform() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestObjectTransform& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && CellId == acRhs.CellId && Position == acRhs.Position && Rotation == acRhs.Rotation && IsReleased == acRhs.IsReleased;
    }

    GameId Id{};
    GameId CellId{};
    Vector3_NetQuantize Position{};

    // Full euler, unlike the actor path's Rotator2_NetQuantize, which carries only pitch and yaw
    // because that is all a walking actor needs. A held object tumbles on every axis.
    glm::vec3 Rotation{};

    // The last message of a hold. Tells the other clients to stop driving the object and hand it back
    // to their own physics.
    bool IsReleased{};
};
