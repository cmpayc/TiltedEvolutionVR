#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>

/**
 * @brief Where a VR player's two palms are, sent for their own character.
 *
 * Positions are **relative to the character's own root**, not world space. A receiver's copy of a player sits
 * at an interpolated position that never quite matches the sender's, so a world position would put the hands
 * off the body by however far the two disagree. Relative coordinates make the receiver reproduce the pose
 * rather than the location, which is also what lets the arm actually reach.
 *
 * Palms only. The elbow and shoulder are solved on the receiver from the palm target, because a bone's parents
 * do not follow it: the chain runs shoulder to elbow to hand, so moving a hand moves nothing else.
 *
 * No rotation. The receiving side currently gives the wrist whatever orientation the forearm hands it, since a
 * VR wand node and a hand bone do not share a rest frame and copying one onto the other twists the palm off
 * the wrist. Adding rotation means measuring that offset first, and there is no point paying for the bytes
 * until then.
 */
struct RequestHandPose final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestHandPose;

    RequestHandPose()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestHandPose() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestHandPose& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && LeftPalm == acRhs.LeftPalm && RightPalm == acRhs.RightPalm && HandsActive == acRhs.HandsActive && EyeHeight == acRhs.EyeHeight;
    }

    // Server entity id of the sender's own character. The server checks it really is theirs before relaying.
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
