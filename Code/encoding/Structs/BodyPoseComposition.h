#pragma once

#include <Structs/BodyPose.h>
#include <Structs/BodyPoseOrigin.h>
#include <span>
#include <cmath>

namespace BodyTracking
{
struct Transform
{
    glm::vec3 Position{};
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
    float Scale{1.f};
};

// AnimationSystem::Serialize sends reference + GetRoomscaleOffset() in XY,
// keeping reference Z. Inputs must be copied in the same post-VRIK sample.
// VRIK's body/root compensation is intentionally NOT part of this origin.
inline Transform CaptureFrame(const glm::vec3& aReference, const glm::vec3& aHmdWorld,
                              const glm::quat& aRotation, float aScale) noexcept
{
    const glm::vec3 roomOffset{RoomscaleOffset(aReference, aHmdWorld), 0.f};
    return {aReference + roomOffset, aRotation, aScale};
}

// Native wire convention: world axes, translated to Tilted's movement origin,
// normalized by the observed 3D scale. No actor Euler conversion is involved.
// The receiver's reference position ALREADY includes the sender's room-scale XY.
inline Transform MovementFrame(const glm::vec3& aMovementPosition, float aScale) noexcept
{
    return {aMovementPosition, {1.f, 0.f, 0.f, 0.f}, aScale};
}
inline Transform SourceMovementFrame(const glm::vec3& aReference, const glm::vec3& aHmdWorld, float aScale) noexcept
{
    return MovementFrame(aReference + glm::vec3{RoomscaleOffset(aReference, aHmdWorld), 0.f}, aScale);
}

inline bool ValidTransform(const Transform& aTransform) noexcept
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(aTransform.Position[i])) return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(aTransform.Rotation[i])) return false;
    const auto norm = glm::dot(aTransform.Rotation, aTransform.Rotation);
    return norm > 0.9f && norm < 1.1f && std::isfinite(aTransform.Scale) && aTransform.Scale > 0.0001f && aTransform.Scale < 1000.f;
}

inline Transform Compose(const Transform& aParent, const Transform& aLocal) noexcept
{
    return {aParent.Position + aParent.Rotation * (aLocal.Position * aParent.Scale),
            glm::normalize(aParent.Rotation * aLocal.Rotation), aParent.Scale * aLocal.Scale};
}

inline Transform RelativeTo(const Transform& aFrame, const Transform& aWorld) noexcept
{
    const auto inverse = glm::conjugate(aFrame.Rotation);
    return {inverse * (aWorld.Position - aFrame.Position) / aFrame.Scale,
            glm::normalize(inverse * aWorld.Rotation), aWorld.Scale / aFrame.Scale};
}

struct Joint
{
    int Parent{-1};
    int Semantic{-1};
    Transform Local{};
    Transform World{};
    bool Owned{};
    uint8_t Visit{};
    // Native adapter marks skin-consumed nodes, including fingers/toes. Losing
    // one must not produce the historical partially updated skin/warped limbs.
    bool RequiredForSkin{};
    bool RequiredPath{};
    bool Pruned{};
};

enum class CompositionFailure { None, Input, Topology, RequiredTransform };
struct CompositionReport
{
    CompositionFailure Failure{};
    size_t PrunedNodes{};
};

// Topology is supplied by the actual receiver. Root and COM carry translation;
// other body joints retain receiver lengths, as in SVMP ComputeRetargetWorld.
// Each joint is composed once, including non-networked fingers, toes and gear.
inline bool ComposeBody(std::span<Joint> aJoints, const Transform& aAnchor, const Pose& aPose,
                        CompositionReport* apReport = nullptr) noexcept
{
    CompositionReport report{};
    const auto fail = [&](CompositionFailure aFailure)
    {
        report.Failure = aFailure;
        if (apReport) *apReport = report;
        return false;
    };
    if (!aPose.IsValid() || !aPose.HasBody() || !ValidTransform(aAnchor) || aJoints.empty() || aJoints.size() > 1024)
        return fail(CompositionFailure::Input);
    uint32_t seen{};
    for (auto& joint : aJoints)
    {
        joint.Visit = 0;
        joint.Owned = false;
        joint.RequiredPath = false;
        joint.Pruned = false;
        if (joint.Parent < -1 || joint.Parent >= static_cast<int>(aJoints.size()) || joint.Semantic < -1)
            return fail(CompositionFailure::Topology);
        if (joint.Semantic >= 0)
        {
            if (joint.Parent < 0 || joint.Semantic >= static_cast<int>(kBoneNames.size()) || (seen & (1u << joint.Semantic)))
                return fail(CompositionFailure::Topology);
            seen |= 1u << joint.Semantic;
        }
    }
    if ((seen & kRequiredMask) != kRequiredMask) return fail(CompositionFailure::Topology);
    // Mark all ancestors of structural/skin joints before pruning anything.
    // Check topology even on optional branches: a cycle is not a collapsed item.
    for (size_t i = 0; i < aJoints.size(); ++i)
    {
        const auto& joint = aJoints[i];
        const bool required = joint.RequiredForSkin || (joint.Semantic >= 0 && (kRequiredMask & (1u << joint.Semantic)));
        int ancestor = static_cast<int>(i);
        size_t depth{};
        while (ancestor >= 0)
        {
            if (++depth > 128) return fail(CompositionFailure::Topology);
            if (required) aJoints[ancestor].RequiredPath = true;
            ancestor = aJoints[ancestor].Parent;
        }
    }
    const auto compose = [&](auto&& self, size_t index, size_t depth) -> bool
    {
        auto& joint = aJoints[index];
        if (joint.Visit == 2) return true;
        if (joint.Visit == 1 || depth > 128) return fail(CompositionFailure::Topology);
        joint.Visit = 1;
        bool invalid = !ValidTransform(joint.Local);
        if (joint.Parent >= 0)
        {
            if (!self(self, joint.Parent, depth + 1)) return false;
            const auto& parent = aJoints[joint.Parent];
            invalid |= parent.Pruned;
            if (!invalid)
            {
                joint.World = Compose(parent.World, joint.Local);
                joint.Owned = parent.Owned;
            }
        }
        else invalid |= !ValidTransform(joint.World);
        if (invalid)
        {
            if (joint.RequiredPath) return fail(CompositionFailure::RequiredTransform);
            joint.Pruned = true;
            joint.Owned = false;
            joint.Visit = 2;
            ++report.PrunedNodes;
            return true;
        }
        // A negative parent is an external anchor whose World was supplied by the engine.
        if (joint.Semantic >= 0 && (aPose.Mask & (1u << joint.Semantic)))
        {
            const auto& bone = aPose.Bones[joint.Semantic];
            if (joint.Semantic != 0 && !joint.Owned) return fail(CompositionFailure::Topology);
            joint.Owned = true;
            joint.World.Rotation = glm::normalize(aAnchor.Rotation * static_cast<const glm::quat&>(bone.Rotation));
            if (joint.Semantic <= 1)
                joint.World.Position = aAnchor.Position + aAnchor.Rotation * (bone.Position * aAnchor.Scale);
        }
        joint.Visit = 2;
        if (!ValidTransform(joint.World))
        {
            if (joint.RequiredPath) return fail(CompositionFailure::RequiredTransform);
            joint.Pruned = true;
            joint.Owned = false;
            ++report.PrunedNodes;
        }
        return true;
    };
    for (size_t i = 0; i < aJoints.size(); ++i)
        if (!compose(compose, i, 0)) return false;
    if (apReport) *apReport = report;
    return true;
}
}
