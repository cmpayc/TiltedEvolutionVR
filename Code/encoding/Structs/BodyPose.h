#pragma once

#include <TiltedCore/Buffer.hpp>
#include <Structs/Quaternion_NetQuantize.h>
#include <Structs/GameId.h>
#include <array>
#include <cstdint>

// The 30 names are the accepted SVMP S2A capture, not traversal-order slots.
// Provenance: TiltedVR-O3-BodyPort-Build.md and prior-body-names.json.
namespace BodyTracking
{
inline constexpr std::array<const char*, 30> kBoneNames = {
    "NPC Root [Root]", "NPC COM [COM ]", "NPC Pelvis [Pelv]",
    "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]",
    "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]",
    "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
    "NPC Neck [Neck]", "NPC Head [Head]", "NPC R Clavicle [RClv]",
    "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]",
    "NPC R MagicNode [RMag]", "NPC R UpperarmTwist1 [RUt1]",
    "NPC R UpperarmTwist2 [RUt2]", "NPC R Pauldron", "NPC L Clavicle [LClv]",
    "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
    "NPC L MagicNode [LMag]", "NPC L UpperarmTwist1 [LUt1]",
    "NPC L UpperarmTwist2 [LUt2]", "NPC L Pauldron"};
inline constexpr uint32_t kKnownMask = (1u << kBoneNames.size()) - 1;
// Magic/pauldron/upperarm-twist extras may be absent; the entire structural body is required.
inline constexpr uint32_t kRequiredMask = kKnownMask & ~((0xfu << 18) | (0xfu << 26));
inline constexpr uint64_t kPresentationDelayMs = 300;
inline constexpr uint64_t kMaxAgeMs = 1000;
inline constexpr uint64_t kSendIntervalMs = 33; // nominal 30 Hz, independent of hand changes/drawn state
inline constexpr float kMaxTranslation = 512.f;

struct BonePose
{
    glm::vec3 Position{};
    Quaternion_NetQuantize Rotation{};
};

struct Pose
{
    // Sequence strictly increases for the entire sender process, including loss,
    // reconnect and 3D replacement. Generation identifies source content ONLY;
    // it never permits Sequence to restart. A new process needs a new connection.
    uint64_t Sequence{};
    uint64_t CaptureTick{};
    uint32_t Generation{};
    uint32_t Mask{};
    std::array<BonePose, kBoneNames.size()> Bones{};
    // Attachment origin relative to the SAME sample's hand bone (R then L),
    // including inverse hand uniform scale. Not controller or actor-root space.
    std::array<BonePose, 2> Grips{};
    // Exact equipped item, in Tilted's existing shared mod namespace. A grip
    // for yesterday's sword must never rotate today's bow during equip delay.
    std::array<GameId, 2> GripItems{};
    uint8_t GripMask{};
    bool Decoded{true};

    bool IsValid() const noexcept;
    bool HasBody() const noexcept { return Mask && (Mask & kRequiredMask) == kRequiredMask; }
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
}
