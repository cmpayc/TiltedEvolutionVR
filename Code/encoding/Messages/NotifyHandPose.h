#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>

/**
 * @brief A remote VR player's palm positions, relayed to everyone near them.
 *
 * Mirrors RequestHandPose, which documents why these are relative to the character's own root and why there is
 * no rotation.
 */
struct NotifyHandPose final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyHandPose;

    NotifyHandPose()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyHandPose() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyHandPose& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && LeftPalm == acRhs.LeftPalm && RightPalm == acRhs.RightPalm && HandsActive == acRhs.HandsActive && EyeHeight == acRhs.EyeHeight;
    }

    // Server entity id of the character these palms belong to.
    uint32_t Id{};

    Vector3_NetQuantize LeftPalm{};
    Vector3_NetQuantize RightPalm{};

    // False while the sender has a weapon out, which is when its hands should be left to the game's own
    // animations. Sent explicitly rather than worked out from the weapon drawn state on the receiving side,
    // because that state is not reliably in sync and gating on it would freeze or release the wrong arms.
    bool HandsActive{true};

    // The sender's real eye height above their own root, in game units, as the headset reports it.
    //
    // The palms are a real world measurement and the character is not the player: a female Skyrim character
    // stands a good deal shorter than a male one, so the same hand height lands proportionally higher up her
    // body. Two players reaching out to shake hands end up a foot apart. The receiver divides by this and
    // multiplies by its own copy of the character's height, which makes the pose a proportion of the body
    // rather than a distance.
    float EyeHeight{0.f};
};
