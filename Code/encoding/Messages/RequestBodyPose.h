#pragma once
#include <Messages/Message.h>
#include <Structs/BodyPose.h>

struct RequestBodyPose final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestBodyPose;
    RequestBodyPose() : ClientMessage(Opcode) {}
    uint32_t Id{};
    BodyTracking::Pose Body{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
};
