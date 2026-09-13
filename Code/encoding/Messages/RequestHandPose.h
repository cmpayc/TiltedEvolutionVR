#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>
#include <Structs/Quaternion_NetQuantize.h>

/**
 * @brief Where a VR player's two palms are, which way they face, and which way the player is looking, sent for
 *        their own character.
 *
 * The head rides in the hand message rather than one of its own. It is measured on the same client, in the same
 * frame, at the same rate, and it needs the same ownership check and the same range filter on the server, so a
 * second message would be a second packet thirty times a second to say what this one already had room for. The
 * two are gated separately: hands stop when a weapon comes out, the head never does.
 *
 * Positions are **relative to the character's own root**, not world space. A receiver's copy of a player sits
 * at an interpolated position that never quite matches the sender's, so a world position would put the hands
 * off the body by however far the two disagree. Relative coordinates make the receiver reproduce the pose
 * rather than the location, which is also what lets the arm actually reach.
 *
 * Palms only. The elbow and shoulder are solved on the receiver from the palm target, because a bone's parents
 * do not follow it: the chain runs shoulder to elbow to hand, so moving a hand moves nothing else.
 *
 * Both the position and the rotation describe the sender's own hand *bone*, not its VR wand node. That matters
 * for each in the same way and for the same reason: the receiver writes them onto its own hand bone, and the
 * wand is neither in the same place as a wrist nor in the same rest frame. Sent as a wand, the position put the
 * receiver's wrist where the sender's controller was, an error of several centimetres pointing in a direction
 * that turned with the hand, and the rotation twisted the palm off the wrist. The sender's
 * "NPC L Hand [LHnd]" is the same bone on the same skeleton as the one the receiver writes, so both transfer
 * with no offset to know.
 *
 * That only holds while something is driving the sender's hand bones from its controllers, which is VRIK's job
 * and VRIK is not required here. HandsRotationValid says whether it was, judged on the sender. When it is false
 * the positions are wand nodes instead, which is the wrong point by a few centimetres and the only tracked
 * position such a client has.
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
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && LeftPalm == acRhs.LeftPalm && RightPalm == acRhs.RightPalm && LeftPalmRotation == acRhs.LeftPalmRotation && RightPalmRotation == acRhs.RightPalmRotation && HandsActive == acRhs.HandsActive && HandsRotationValid == acRhs.HandsRotationValid && EyeHeight == acRhs.EyeHeight && HeadRotation == acRhs.HeadRotation && HeadRotationValid == acRhs.HeadRotationValid;
    }

    // Server entity id of the sender's own character. The server checks it really is theirs before relaying.
    uint32_t Id{};

    Vector3_NetQuantize LeftPalm{};
    Vector3_NetQuantize RightPalm{};

    Quaternion_NetQuantize LeftPalmRotation{};
    Quaternion_NetQuantize RightPalmRotation{};

    // False while the sender has a weapon out, which is when its hands should be left to the game's own
    // animations. Sent explicitly rather than worked out from the weapon drawn state on the receiving side,
    // because that state is not reliably in sync and gating on it would freeze or release the wrong arms.
    bool HandsActive{true};

    /**
     * @brief Whether the two rotations are the sender's tracked wrists rather than its idle animation.
     *
     * The orientation is read off the sender's own hand bones, which only follow the controllers while VRIK, or
     * something like it, is driving them. VRIK is not a requirement of this fork, so on a client without it
     * those bones carry whatever the third person animation is playing: a perfectly valid rotation that has
     * nothing to do with where the player's hands are pointing.
     *
     * The sender tests this rather than declaring it, by checking its wrist actually sits at its wand, and says
     * so here. A receiver seeing false leaves the wrist to the arm solve, which is what it did before rotation
     * existed at all.
     */
    bool HandsRotationValid{false};

    // The sender's real eye height above their own root, in game units, as the headset reports it.
    //
    // The palms are a real world measurement and the character is not the player: a female Skyrim character
    // stands a good deal shorter than a male one, so the same hand height lands proportionally higher up her
    // body. Two players reaching out to shake hands end up a foot apart. The receiver divides by this and
    // multiplies by its own copy of the character's height, which makes the pose a proportion of the body
    // rather than a distance.
    //
    // Rotation needs none of this. A rotation is scale free, so it carries across bodies of different sizes
    // unchanged.
    float EyeHeight{0.f};

    /**
     * @brief Which way the sender's headset is pointing, in the same root relative frame as the palms.
     *
     * The headset node rather than the head bone, which is the opposite choice to the palms and is right for
     * the opposite reason. A palm comes from the hand bone because the receiver writes a hand bone and the two
     * share a rest frame. A head has no such luxury on a client without VRIK: nothing drives its head bone from
     * the headset there, so the bone would carry the idle animation and every such player would be reported as
     * staring straight ahead. The headset node is tracked on every VR client whatever else is installed.
     *
     * That leaves the offset between the headset's frame and a head bone's, which would be a constant nobody
     * has measured. SkyrimVR builds the node in the character's own axes, X right, Y forward, Z up, so a
     * headset held level and facing along the body reads as no rotation at all and the offset is the identity.
     * The sender proves that against the node before trusting it, rather than assuming it, and reports the
     * verdict in HeadRotationValid.
     *
     * Relative to the root, not to the world, for the same reason the palms are. A receiver's copy of this
     * player stands at an interpolated rotation that never quite matches, and SkyrimVR turns the body with the
     * headset in the first place, so a world orientation would land on a body that had already been turned and
     * count the yaw twice. Relative, the body carries the yaw it already has and this carries what is left:
     * the pitch, the roll, and however far the head is turned ahead of the body.
     */
    Quaternion_NetQuantize HeadRotation{};

    /**
     * @brief Whether HeadRotation came from a headset node this client could read and verify.
     *
     * False from a client that is not in VR at all, and false when the node is not where this build expects it
     * or does not carry the axes the frame above depends on. A receiver seeing false leaves the head to the
     * game's own animation, which is what it did before head rotation was synced.
     */
    bool HeadRotationValid{false};
};
