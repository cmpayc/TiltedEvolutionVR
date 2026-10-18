#include <TiltedOnlinePCH.h>
#include <Havok/BodyReferenceSkeleton.h>
#include <Actor.h>
#include <BSAnimationGraphManager.h>
#include <NetImmerse/NiNode.h>
#include <cstring>
#include <limits>

namespace BodyTracking
{
#if TP_SKYRIMVR
namespace
{
// VR 1.4.15 reflection records (pinned image SHA 8daf0519...46d4):
// hkbCharacter.setup +50; hkbCharacterSetup.animationSkeleton +20;
// hkaSkeleton parentIndices +18, bones +28, referencePose +38.
// BShkbAnimationGraph character +C0, holder +210, root +218 are separately
// verified against the VR constructor and the pinned CommonLib layout.
// See round4-member-records.json and the O3 build document for full provenance.
struct HkArrayView { uintptr_t Data; int32_t Size; uint32_t CapacityAndFlags; };
struct GraphView
{
    uint8_t Prefix[0x110];
    uintptr_t Setup;
    uint8_t ToHolder[0x210 - 0x118];
    uintptr_t Holder;
    uintptr_t Root;
};
struct SetupView { uint8_t Prefix[0x20]; uintptr_t AnimationSkeleton; };
struct SkeletonView
{
    uint8_t Prefix[0x18];
    HkArrayView Parents, Bones, ReferencePose;
};
struct BoneView { uintptr_t Name; uint8_t Locked; uint8_t Padding[7]; };
struct PoseView { float Translation[4], Rotation[4], Scale[4]; };
static_assert(sizeof(HkArrayView) == 0x10 && sizeof(BoneView) == 0x10 && sizeof(PoseView) == 0x30);
static_assert(offsetof(GraphView, Setup) == 0x110 && offsetof(GraphView, Holder) == 0x210 && offsetof(GraphView, Root) == 0x218);
static_assert(offsetof(SetupView, AnimationSkeleton) == 0x20);
static_assert(offsetof(SkeletonView, Parents) == 0x18 && offsetof(SkeletonView, Bones) == 0x28 && offsetof(SkeletonView, ReferencePose) == 0x38);

// Cache queried memory regions only for this locked snapshot, not between
// frames. Names are copied in region-sized chunks, avoiding a syscall/byte.
class BoundedRead
{
public:
    bool Copy(uintptr_t aSource, void* apDestination, size_t aBytes)
    {
        if (!aSource || aBytes > std::numeric_limits<uintptr_t>::max() - aSource) return false;
        auto* destination = static_cast<uint8_t*>(apDestination);
        while (aBytes)
        {
            uintptr_t end{};
            if (!RegionEnd(aSource, end)) return false;
            const auto count = std::min(aBytes, static_cast<size_t>(end - aSource));
            std::memcpy(destination, reinterpret_cast<const void*>(aSource), count);
            destination += count;
            aSource += count;
            aBytes -= count;
        }
        return true;
    }
    template<class T> bool Read(uintptr_t aSource, T& aValue) { return Copy(aSource, &aValue, sizeof(T)); }
    bool Name(uintptr_t aSource, std::string& aName, bool aTagged = true)
    {
        // hkStringPtr's low bit records owned storage, not part of the address.
        if (aTagged) aSource &= ~uintptr_t{1};
        aName.clear();
        std::array<char, kMaxBoneNameBytes> text{};
        size_t copied{};
        while (copied < text.size())
        {
            uintptr_t end{};
            if (!RegionEnd(aSource, end)) return false;
            const auto count = std::min(text.size() - copied, static_cast<size_t>(end - aSource));
            std::memcpy(text.data() + copied, reinterpret_cast<const void*>(aSource), count);
            const auto* terminator = static_cast<const char*>(std::memchr(text.data() + copied, 0, count));
            if (terminator)
            {
                aName.assign(text.data(), static_cast<size_t>(terminator - text.data()));
                return !aName.empty();
            }
            copied += count;
            aSource += count;
        }
        return false;
    }
private:
    bool RegionEnd(uintptr_t aSource, uintptr_t& aEnd)
    {
        if (!aSource) return false;
        for (const auto& region : Regions)
            if (aSource >= region.first && aSource < region.second) { aEnd = region.second; return true; }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(aSource), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto access = info.Protect & 0xff;
        if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
            access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
        const auto start = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (info.RegionSize > std::numeric_limits<uintptr_t>::max() - start) return false;
        aEnd = start + info.RegionSize;
        if (aEnd <= aSource) return false;
        Regions[Next++ % Regions.size()] = {start, aEnd};
        return true;
    }
    std::array<std::pair<uintptr_t, uintptr_t>, 16> Regions{};
    size_t Next{};
};

bool ValidArray(const HkArrayView& aArray, int32_t aCount)
{
    return aArray.Data && aCount > 0 && aCount <= static_cast<int32_t>(kMaxReferenceBones) &&
        aArray.Size == aCount && (aArray.CapacityAndFlags & 0x3fffffffU) >= static_cast<uint32_t>(aCount);
}

struct ManagerLease
{
    BSAnimationGraphManager* Pointer{};
    ~ManagerLease() { if (Pointer) Pointer->Release(); }
};
}
#endif

ReferenceFailure ReadBodyReferenceSkeleton(Actor& aActor, NiNode* apExpectedRoot,
                                          uint64_t aGeneration, ReferenceSkeleton& aOutput,
                                          ReferenceDiagnostic* apDiagnostic, bool aReuse)
{
    bool accepted{};
    struct ClearOnFailure
    {
        ReferenceSkeleton& Output;
        bool& Accepted;
        ~ClearOnFailure() { if (!Accepted) Output = {}; }
    } clear{aOutput, accepted};
    if (apDiagnostic) *apDiagnostic = {0, 0, reinterpret_cast<uintptr_t>(&aActor),
        reinterpret_cast<uintptr_t>(&aActor.animationGraphHolder), reinterpret_cast<uintptr_t>(apExpectedRoot)};
#if !TP_SKYRIMVR
    return ReferenceFailure::UnsupportedRuntime;
#else
    if (!apExpectedRoot || !aGeneration || aActor.GetNiNode() != apExpectedRoot) return ReferenceFailure::ActorIdentity;
    // Acquire in the actor update phase; retain the root while the additional
    // graph/setup/skeleton view is followed. This does not claim that a raw
    // pointer can be safely acquired from an arbitrary game thread.
    struct RootLease
    {
        NiNode* Root;
        ~RootLease() { Root->DecRef(); }
    } rootLease{apExpectedRoot};
    apExpectedRoot->IncRef();
    ManagerLease lease{};
    const bool acquired = aActor.animationGraphHolder.GetBSAnimationGraph(&lease.Pointer);
    // Release a non-null result even when the holder reports failure.
    if (!acquired || !lease.Pointer) return ReferenceFailure::Manager;
    ReferenceSkeleton snapshot{};
    {
        // The lock dies before the retained manager on EVERY return path.
        BSScopedLock<BSRecursiveLock> lock{lease.Pointer->lock};
        auto& manager = *lease.Pointer;
        const auto& graphs = manager.animationGraphs;
        const auto index = manager.animationGraphIndex;
        if (!graphs.size || graphs.size > 64 || index >= graphs.size) return ReferenceFailure::Graph;
        BoundedRead read;
        std::vector<GraphView> views(graphs.size);
        std::vector<ReferenceGraphIdentity> identities(graphs.size);
        if (apDiagnostic)
        {
            apDiagnostic->GraphCount = graphs.size;
            apDiagnostic->ActiveGraph = index;
            apDiagnostic->Graphs.resize(graphs.size);
        }
        if (graphs.capacity < 0 && (graphs.size != 1 || index != 0)) return ReferenceFailure::Graph;
        if (graphs.capacity >= 0 && graphs.capacity < static_cast<int32_t>(graphs.size)) return ReferenceFailure::Graph;
        const auto array = reinterpret_cast<uintptr_t>(graphs.data);
        if (graphs.capacity >= 0 && array > UINTPTR_MAX - graphs.size * sizeof(uintptr_t)) return ReferenceFailure::Graph;
        for (uint32_t slot = 0; slot < graphs.size; ++slot)
        {
            uintptr_t address{};
            const bool slotRead = graphs.capacity < 0 ? (address = array, true) : read.Read(array + slot * sizeof(uintptr_t), address);
            auto& identity = identities[slot];
            identity.Address = address;
            identity.Readable = slotRead && read.Read(address, views[slot]);
            if (identity.Readable)
            {
                identity.Holder = views[slot].Holder;
                identity.Root = views[slot].Root;
            }
            if (apDiagnostic) apDiagnostic->Graphs[slot].Identity = identity;
        }
        const int selected = SelectReferenceGraph(identities, reinterpret_cast<uintptr_t>(&aActor), reinterpret_cast<uintptr_t>(apExpectedRoot));
        if (apDiagnostic)
        {
            apDiagnostic->SelectedGraph = selected;
            const auto& reported = identities[selected >= 0 ? selected : index];
            apDiagnostic->StoredHolder = reported.Holder;
            apDiagnostic->StoredRoot = reported.Root;
            // Names are diagnostic only; a missing name cannot select a graph.
            if (selected < 0) for (auto& item : apDiagnostic->Graphs)
            {
                uintptr_t name{};
                if (!item.Identity.Readable || item.Identity.Root > UINTPTR_MAX - 0x10 ||
                    !read.Read(item.Identity.Root + 0x10, name) || !read.Name(name, item.RootName, false)) item.RootName = "<unreadable>";
            }
        }
        if (selected < 0) return selected == -2 ? ReferenceFailure::Graph : ReferenceFailure::ActorIdentity;
        const auto graphAddress = identities[selected].Address;
        const auto& graph = views[selected];
        SetupView setup{};
        SkeletonView skeleton{};
        if (!read.Read(graph.Setup, setup) || !read.Read(setup.AnimationSkeleton, skeleton)) return ReferenceFailure::Unreadable;
        const auto count = skeleton.Bones.Size;
        if (!ValidArray(skeleton.Parents, count) || !ValidArray(skeleton.Bones, count) || !ValidArray(skeleton.ReferencePose, count))
            return ReferenceFailure::Arrays;
        if (aReuse && aOutput.Generation == aGeneration && aOutput.GraphIdentity == graphAddress &&
            aOutput.SkeletonIdentity == setup.AnimationSkeleton && aOutput.RootIdentity == graph.Root &&
            aOutput.ParentsIdentity == skeleton.Parents.Data && aOutput.BonesIdentity == skeleton.Bones.Data &&
            aOutput.PoseIdentity == skeleton.ReferencePose.Data && aOutput.Bones.size() == static_cast<size_t>(count) &&
            aActor.GetNiNode() == apExpectedRoot)
        {
            accepted = true;
            return ReferenceFailure::None;
        }
        std::vector<int16_t> parents(count);
        std::vector<BoneView> bones(count);
        std::vector<PoseView> poses(count);
        if (!read.Copy(skeleton.Parents.Data, parents.data(), parents.size() * sizeof(int16_t)) ||
            !read.Copy(skeleton.Bones.Data, bones.data(), bones.size() * sizeof(BoneView)) ||
            !read.Copy(skeleton.ReferencePose.Data, poses.data(), poses.size() * sizeof(PoseView))) return ReferenceFailure::Unreadable;
        snapshot.Bones.resize(count);
        for (int32_t i = 0; i < count; ++i)
        {
            auto& bone = snapshot.Bones[i];
            if (!read.Name(bones[i].Name, bone.Name) || bones[i].Locked > 1) return ReferenceFailure::Names;
            bone.Parent = parents[i];
            bone.TranslationLocked = bones[i].Locked != 0;
            if (!ReferenceTransform(poses[i].Translation, poses[i].Rotation, poses[i].Scale, bone.Local))
                return ReferenceFailure::Transform;
        }
        if (const auto failure = ValidateReferenceSkeleton(snapshot); failure != ReferenceFailure::None) return failure;
        if (aActor.GetNiNode() != apExpectedRoot) return ReferenceFailure::ActorIdentity;
        snapshot.GraphIdentity = graphAddress;
        snapshot.SkeletonIdentity = setup.AnimationSkeleton;
        snapshot.RootIdentity = graph.Root;
        snapshot.ParentsIdentity = skeleton.Parents.Data;
        snapshot.BonesIdentity = skeleton.Bones.Data;
        snapshot.PoseIdentity = skeleton.ReferencePose.Data;
        snapshot.Generation = aGeneration;
    }
    aOutput = std::move(snapshot);
    accepted = true;
    return ReferenceFailure::None;
#endif
}
}
