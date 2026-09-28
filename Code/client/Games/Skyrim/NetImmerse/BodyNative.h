#pragma once

#include <Structs/BodyReferenceSkeleton.h>
#include <Structs/BodyPoseSource.h>
#include <NetImmerse/BodySkinReader.h>
#include <memory>

struct Actor;
struct NiNode;
struct PlayerCharacter;

namespace BodyTracking
{
namespace PostPass { struct Snapshot; }
enum class NativeFailure { None, Unreadable, Topology, RequiredBody, SkinAlias, ReferenceBinding, Transform, Changed };
struct GripReport
{
    uint8_t Applied{}, Unmapped{}, Missing{}, Ambiguous{}, Pruned{};
};
struct NativeDiagnostic
{
    std::string Reason, Geometry, Bone;
    int Slot{-1};
    uintptr_t BonePointer{}, WorldPointer{};
    ReferenceBindDiagnostic Binding;
    SkinReadDiagnostic Skin;
};

// One callback's retained native view. No pointer in this object is published
// to another phase/thread. The value-only animation reference is separate.
class NativeBody
{
public:
    NativeBody();
    ~NativeBody();
    NativeBody(const NativeBody&) = delete;
    NativeBody& operator=(const NativeBody&) = delete;
    NativeFailure Read(NiNode* apRoot, bool aRequireSkin);
    NativeFailure Bind(const ReferenceSkeleton& aReference);
    // apSnapshot (optional) receives what this invocation wrote, in flattened-tree terms, for the post-pass
    // re-apply (Services/BodyPostPass.h). Filled only when the writes committed.
    NativeFailure Apply(const Pose& aPose, const Transform& aAnchor, const std::array<uint32_t, 2>& aItems, float& aHandError, GripReport* apGripReport = nullptr, PostPass::Snapshot* apSnapshot = nullptr);
    bool Capture(const Transform& aFrame, Pose& aPose) const;
    bool ReadHmdWorld(PlayerCharacter* apPlayer, Transform& aWorld);
    void CaptureGrips(SourceSample& aSample) const;
    Transform RootWorld() const;
    Transform BoneWorld(int aSemantic) const;
    uint64_t Content(bool aSource = false) const;
    NativeReadStats Stats() const;
    uint64_t StableMicros() const;
    const NativeDiagnostic& Diagnostic() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Cold-start only, immediately after synchronous LoadScriptExtender(). Never
// retries registration from UpdateEvent. Storage intentionally survives CRT.
void RegisterBodyCaptureAtStartup();
void EnableBodyCapture(bool aEnable);
void ReportBodyCaptureLoss();
void StopBodyCapture();
SourceSample ReadBodyCapture();
uint64_t BodySteadyMs();
}
