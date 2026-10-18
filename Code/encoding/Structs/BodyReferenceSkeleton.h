#pragma once

#include <Structs/BodyPoseComposition.h>
#include <algorithm>
#include <string>
#include <vector>
#include <string_view>
#include <bit>

namespace BodyTracking
{
inline constexpr size_t kMaxReferenceBones = 1024;
inline constexpr size_t kMaxBoneNameBytes = 128;

struct ReferenceBone
{
    std::string Name;
    int Parent{-1};
    Transform Local{};
    bool TranslationLocked{};
};

// Values only. These identity tags must never be dereferenced after the reader
// returns. The native adapter must invalidate on graph/3D/content replacement;
// pointer equality alone is not a lifetime or cache-validity guarantee.
struct ReferenceSkeleton
{
    std::vector<ReferenceBone> Bones;
    std::array<int, kBoneNames.size()> SemanticIndices{};
    uintptr_t GraphIdentity{};
    uintptr_t SkeletonIdentity{};
    uintptr_t RootIdentity{};
    uintptr_t ParentsIdentity{}, BonesIdentity{}, PoseIdentity{};
    uint64_t Generation{};
};

enum class ReferenceFailure
{
    None, UnsupportedRuntime, ActorIdentity, Manager, Graph, Unreadable,
    Arrays, Names, Topology, Transform, MissingBody, TranslationPolicy
};

struct ReferenceBindDiagnostic
{
    ReferenceFailure Failure{};
    std::string Bone, LiveParent, ReferenceParent;
    float LiveScale{}, ReferenceScale{};
};
struct ReferenceGraphIdentity { uintptr_t Address{}, Holder{}, Root{}; bool Readable{}; };
// No first/third-person naming assumption; reject ambiguous exact matches.
inline int SelectReferenceGraph(std::span<const ReferenceGraphIdentity> graphs, uintptr_t holder, uintptr_t root)
{
    if (!holder || !root) return -1;
    int selected = -1;
    for (size_t i = 0; i < graphs.size(); ++i)
        if (graphs[i].Readable && graphs[i].Address && graphs[i].Holder == holder && graphs[i].Root == root)
        {
            if (selected >= 0) return -2;
            selected = static_cast<int>(i);
        }
    return selected;
}

// Raw hkQsTransform is translation4, quaternion XYZW, scale4 (0x30 bytes).
// Havok animation-skeleton translations are already in model units. This path
// does not apply the physics-world unit conversion or invent a scale factor.
inline bool ReferenceTransform(const float* apTranslation, const float* apQuaternion,
                               const float* apScale, Transform& aOutput) noexcept
{
    if (!std::isfinite(apScale[1]) || !std::isfinite(apScale[2])) return false;
    const float tolerance = 1e-5f * std::max(1.f, std::abs(apScale[0]));
    if (std::abs(apScale[0] - apScale[1]) > tolerance ||
        std::abs(apScale[0] - apScale[2]) > tolerance) return false;
    aOutput = {{apTranslation[0], apTranslation[1], apTranslation[2]},
               {apQuaternion[3], apQuaternion[0], apQuaternion[1], apQuaternion[2]}, apScale[0]};
    return ValidTransform(aOutput);
}

// Validate the actual loaded skeleton, not a table selected by race or file
// name. Optional wire bones remain optional; if present they obey the same
// fixed-translation rule as other selected bones. Only Root and COM may move.
inline ReferenceFailure ValidateReferenceSkeleton(ReferenceSkeleton& aSkeleton)
{
    aSkeleton.SemanticIndices.fill(-1);
    if (aSkeleton.Bones.empty() || aSkeleton.Bones.size() > kMaxReferenceBones)
        return ReferenceFailure::Arrays;
    uint32_t found{};
    for (size_t i = 0; i < aSkeleton.Bones.size(); ++i)
    {
        const auto& bone = aSkeleton.Bones[i];
        if (bone.Name.empty() || bone.Name.size() >= kMaxBoneNameBytes || bone.Name.find('\0') != std::string::npos)
            return ReferenceFailure::Names;
        for (size_t j = 0; j < i; ++j)
            if (aSkeleton.Bones[j].Name == bone.Name) return ReferenceFailure::Names;
        // First slice requires a topologically ordered reference skeleton.
        if (bone.Parent < -1 || bone.Parent >= static_cast<int>(i)) return ReferenceFailure::Topology;
        if (!ValidTransform(bone.Local)) return ReferenceFailure::Transform;
        for (size_t semantic = 0; semantic < kBoneNames.size(); ++semantic)
        {
            if (bone.Name != kBoneNames[semantic]) continue;
            if (bone.TranslationLocked != (semantic > 1)) return ReferenceFailure::TranslationPolicy;
            aSkeleton.SemanticIndices[semantic] = static_cast<int>(i);
            found |= 1u << semantic;
        }
    }
    if ((found & kRequiredMask) != kRequiredMask) return ReferenceFailure::MissingBody;
    // The renderer must additionally match each selected bone's actual parent
    // by name, establish animation/render scale correspondence, and cover all
    // skin aliases before using these local transforms. Read success alone is
    // not permission to write an active rig or a proven calibration result.
    return ReferenceFailure::None;
}

// This is the actual native binding operation, also exercised with retail
// reference fixtures and deliberately poisoned arm/leg translations in tests.
// It neither edits engine locals nor infers lengths from a live animation.
inline ReferenceFailure BindReferenceLocals(std::span<Joint> joints,
    std::span<const std::string_view> names, const ReferenceSkeleton& reference, ReferenceBindDiagnostic* diagnostic = nullptr)
{
    if (diagnostic) *diagnostic = {};
    const auto fail = [&](ReferenceFailure reason) { if (diagnostic) diagnostic->Failure = reason; return reason; };
    if (joints.size() != names.size() || joints.size() > kMaxReferenceBones) return fail(ReferenceFailure::Arrays);
    for (size_t i = 0; i < joints.size(); ++i)
        if (joints[i].Parent < -1 || joints[i].Parent >= static_cast<int>(joints.size()))
        {
            if (diagnostic) { diagnostic->Bone = names[i]; diagnostic->LiveScale = joints[i].Local.Scale; diagnostic->LiveParent = "<invalid index>"; }
            return fail(ReferenceFailure::Topology);
        }
    for (size_t i = 0; i < joints.size(); ++i)
    {
        auto& joint = joints[i];
        const auto found = std::find_if(reference.Bones.begin(), reference.Bones.end(), [&](const auto& bone) { return bone.Name == names[i]; });
        if (found == reference.Bones.end())
        {
            if (joint.Semantic >= 0) { if (diagnostic) diagnostic->Bone = names[i]; return fail(ReferenceFailure::MissingBody); }
            continue; // actual render-only tail/gear keeps its local transform
        }
        const auto& bone = *found;
        const auto failBone = [&](ReferenceFailure reason)
        {
            if (diagnostic)
            {
                diagnostic->Bone = names[i];
                diagnostic->LiveScale = joint.Local.Scale;
                diagnostic->ReferenceScale = bone.Local.Scale;
                diagnostic->LiveParent = joint.Parent >= 0 ? std::string(names[joint.Parent]) : "<none>";
                diagnostic->ReferenceParent = bone.Parent >= 0 && bone.Parent < static_cast<int>(reference.Bones.size()) ? reference.Bones[bone.Parent].Name : "<none/invalid>";
            }
            return fail(reason);
        };
        if (bone.Parent >= static_cast<int>(reference.Bones.size()) || bone.Parent < -1 ||
            (bone.Parent >= 0 && (joint.Parent < 0 || names[joint.Parent] != reference.Bones[bone.Parent].Name))) return failBone(ReferenceFailure::Topology);
        if (!std::isfinite(joint.Local.Scale) || std::abs(joint.Local.Scale - bone.Local.Scale) > .0001f) return failBone(ReferenceFailure::Transform);
        if (bone.TranslationLocked) joint.Local.Position = bone.Local.Position;
    }
    if (diagnostic) *diagnostic = {};
    return ReferenceFailure::None;
}

inline bool CompleteSkinDetails(const ReferenceSkeleton& reference, std::span<const std::string_view> consumed, std::string* missing = nullptr)
{
    std::array<bool, 6> groups{};
    constexpr std::array<std::string_view, 6> prefixes{"NPC L Finger", "NPC R Finger",
        "NPC L ForearmTwist", "NPC R ForearmTwist", "NPC L Toe0", "NPC R Toe0"};
    for (const auto& bone : reference.Bones)
        for (size_t group = 0; group < prefixes.size(); ++group)
            if (bone.Name.starts_with(prefixes[group]))
            {
                if (std::find(consumed.begin(), consumed.end(), bone.Name) == consumed.end()) { if (missing) *missing = bone.Name; return false; }
                groups[group] = true;
            }
    return std::all_of(groups.begin(), groups.end(), [](bool found) { return found; });
}

// Cache identity for original reference data, NOT a reusable native write map.
// Canonical semantic order excludes cosmetic child insertion/reordering/scale.
// Native skin/attachment maps are still rebuilt and checked on every read; their
// changing arrays do not invalidate immutable Havok lengths. The Havok reader
// independently checks graph/root/skeleton/array identity on each update.
inline uint64_t ReferenceContentIdentity(std::span<const Joint> joints,
    std::span<const uint64_t> identities, float rootScale)
{
    if (joints.size() != identities.size()) return 0;
    std::array<int, kBoneNames.size()> selected;
    selected.fill(-1);
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const int semantic = joints[i].Semantic;
        if (semantic < 0 || semantic >= static_cast<int>(selected.size()) || !(kRequiredMask & (1u << semantic))) continue;
        if (selected[semantic] != -1) return 0;
        selected[semantic] = static_cast<int>(i);
    }
    uint64_t hash = 1469598103934665603ULL;
    const auto fold = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    fold(std::bit_cast<uint32_t>(rootScale));
    for (size_t semantic = 0; semantic < selected.size(); ++semantic)
    {
        if (!(kRequiredMask & (1u << semantic))) continue;
        const int index = selected[semantic];
        if (index < 0) return 0;
        const int parent = joints[index].Parent;
        if (parent < 0 || parent >= static_cast<int>(identities.size())) return 0;
        fold(identities[index]);
        fold(identities[parent]);
        fold(std::bit_cast<uint32_t>(joints[index].Local.Scale));
    }
    return hash ? hash : 1;
}
}
