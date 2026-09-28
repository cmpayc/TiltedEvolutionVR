#pragma once
#include <Messages/Message.h>
#include <Structs/BodyPose.h>

struct NotifyBodyPose final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyBodyPose;
    NotifyBodyPose() : ServerMessage(Opcode) {}
    uint32_t Id{};
    uint64_t ServerTick{};
    BodyTracking::Pose Body{};
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
};
