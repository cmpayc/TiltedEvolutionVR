#include <TiltedOnlinePCH.h>
#include <NetImmerse/BodyNative.h>
#include <Services/BodyPostPass.h>
#include <NetImmerse/BodyReadMemory.h>
#include <NetImmerse/BodyProbeMemory.h>
#include <Structs/BodyPoseAttachment.h>
#include <NetImmerse/NiNode.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <ModCompat/HiggsAPI.h>
#include <ModCompat/openvr.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <bit>

namespace BodyTracking
{
namespace
{
// VR 1.4.15. Ni world/local and flattened layout: Tilted PROGRESS session 9;
// skin consumers, as the earlier standalone plugin's completed alias mapping established.
// Bounds are checked per invocation, never cached across native lifetimes.
// The readability primitive lives in BodyProbeMemory.h (rpm by default, vq by the startup switch).
using Memory = BodyReadMemory<RuntimeMemory>;
struct MicrosScope
{
    uint64_t& Value;
    std::chrono::steady_clock::time_point Begin{std::chrono::steady_clock::now()};
    ~MicrosScope() { Value += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count(); }
};

struct NiTransformView { float R[9], T[3], S; };
static_assert(sizeof(NiTransformView) == 0x34);
Transform Decode(const NiTransformView& raw)
{
    glm::mat3 rotation{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) rotation[col][row] = raw.R[row * 3 + col];
    bool valid = std::isfinite(glm::determinant(rotation)) && std::abs(glm::determinant(rotation) - 1.f) < .01f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            valid &= std::abs(glm::dot(rotation[i], rotation[j]) - (i == j ? 1.f : 0.f)) < .01f;
    return {{raw.T[0], raw.T[1], raw.T[2]}, valid ? glm::normalize(glm::quat_cast(rotation)) :
        glm::quat{NAN, NAN, NAN, NAN}, raw.S};
}
bool ReadTransform(Memory& memory, uintptr_t p, Transform& value)
{
    NiTransformView raw{};
    if (!memory.Read(p, raw)) return false;
    value = Decode(raw);
    return true;
}
NiTransformView Encode(const Transform& transform)
{
    NiTransformView raw{};
    const auto matrix = glm::mat3_cast(transform.Rotation);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) raw.R[row * 3 + col] = matrix[col][row];
    for (int i = 0; i < 3; ++i) raw.T[i] = transform.Position[i];
    raw.S = transform.Scale;
    return raw;
}
int Semantic(const std::string& name)
{
    for (size_t i = 0; i < kBoneNames.size(); ++i) if (name == kBoneNames[i]) return static_cast<int>(i);
    return -1;
}
struct Pin
{
    NiAVObject* Pointer;
    explicit Pin(uintptr_t p) : Pointer(reinterpret_cast<NiAVObject*>(p)) { Pointer->IncRef(); }
    ~Pin() { Pointer->DecRef(); }
};
}

struct NativeBody::Impl
{
    struct Check { uintptr_t Address; std::array<uint8_t, 16> Bytes{}; size_t Size; };
    struct Node { uintptr_t Object{}; std::string Name; std::vector<uintptr_t> Worlds; uintptr_t Local{}; int Parent{-1}; bool Skin{}; int FlatParent{-2}; uint64_t Identity{};
                  int EntryIndex{-1}; int TreeSlot{-1}; uintptr_t EntryName{}; uintptr_t Table{};
                  struct Alias { int Index; int TreeSlot; uintptr_t Name; }; std::vector<Alias> Aliases; }; // every flattened entry that references this node
    struct TreeInfo { int Node{-1}; uintptr_t Data{}; uint32_t Count{}; std::array<float, 3> Root{}; };
    Memory ReadMemory;
    NativeDiagnostic Detail;
    SkinReadStats SkinStats;
    uint64_t StableMicros{}; // Capture's Stable() pass only; read by the capture callback
    int LastGrip[2]{-1, -1}; // joint index Apply matched per hand this invocation
    std::vector<std::unique_ptr<Pin>> Pins;
    std::vector<Node> Nodes;
    std::vector<Joint> Joints;
    std::vector<Check> Checks;
    std::unordered_map<uintptr_t, int> Objects;
    std::vector<int> Trees, Geometry;
    std::vector<TreeInfo> TreeInfos;

    template<class T> bool Guard(uintptr_t address, T& value)
    {
        static_assert(sizeof(T) <= 16);
        if (!ReadMemory.Read(address, value) || Checks.size() >= 16384) return false;
        Check check{address, {}, sizeof(T)};
        std::memcpy(check.Bytes.data(), &value, sizeof(T));
        Checks.push_back(check);
        return true;
    }
    bool Stable()
    {
        for (const auto& check : Checks)
            if (!ReadMemory.Range(check.Address, check.Size) ||
                std::memcmp(reinterpret_cast<void*>(check.Address), check.Bytes.data(), check.Size)) return false;
        return true;
    }
    int Visit(uintptr_t object, int parent, size_t depth)
    {
        if (auto found = Objects.find(object); found != Objects.end()) return found->second;
        if (!object || depth > 128 || Nodes.size() >= 1024 || !ReadMemory.Kind(object, "NiAVObject")) return -1;
        Pins.push_back(std::make_unique<Pin>(object));
        uintptr_t table{}, name{}, nativeParent{};
        std::string text;
        if (!Guard(object, table) || !Guard(object + 0x10, name) || !Guard(object + 0x30, nativeParent) || !ReadMemory.Name(name, text)) return -1;
        const int index = static_cast<int>(Nodes.size());
        Objects.emplace(object, index);
        Nodes.push_back({object, text, {object + 0x7c}, object + 0x48, parent});
        Nodes.back().Identity = ((object ^ table) * 1099511628211ULL ^ name) * 1099511628211ULL;
        Nodes.back().Table = table;
        if (ReadMemory.Kind(object, "BSFlattenedBoneTree")) Trees.push_back(index);
        if (ReadMemory.Kind(object, "BSGeometry")) Geometry.push_back(index);
        if (ReadMemory.Kind(object, "NiNode"))
        {
            uintptr_t data{};
            uint16_t capacity{}, end{}, size{};
            if (!Guard(object + 0x140, data) || !Guard(object + 0x148, capacity) ||
                !Guard(object + 0x14a, end) || !Guard(object + 0x14c, size) || end > capacity || size > end || capacity > 1024) return -1;
            if (end && !ReadMemory.Range(data, end * sizeof(uintptr_t))) return -1;
            for (size_t i = 0; i < end; ++i)
            {
                uintptr_t child{};
                if (!Guard(data + i * 8, child)) return -1;
                if (child && Visit(child, index, depth + 1) < 0) return -1;
            }
        }
        return index;
    }
    NativeFailure SkinFailure(const char* reason, int geometry = -1, int slot = -1, uintptr_t bone = 0, uintptr_t world = 0)
    {
        Detail.Reason = reason;
        if (geometry >= 0) Detail.Geometry = Nodes[geometry].Name;
        Detail.Slot = slot; Detail.BonePointer = bone; Detail.WorldPointer = world;
        return NativeFailure::SkinAlias;
    }
    NativeFailure Read(NiNode* root, bool requireSkin)
    {
        if (Visit(reinterpret_cast<uintptr_t>(root), -1, 0) != 0) return NativeFailure::Unreadable;
        std::unordered_set<uintptr_t> flatAliases;
        for (size_t treeSlot = 0; treeSlot < Trees.size(); ++treeSlot)
        {
            const int treeIndex = Trees[treeSlot];
            const auto tree = Nodes[treeIndex].Object;
            uint32_t count{}, populated{};
            uintptr_t data{};
            if (!Guard(tree + 0x150, count) || !Guard(tree + 0x154, populated) || !Guard(tree + 0x158, data) ||
                !count || count > 1024 || populated > count || !data) return NativeFailure::Topology;
            if (!ReadMemory.Range(data, count * 0x80)) return NativeFailure::Unreadable;
            {
                TreeInfo info{treeIndex, data, count, {}};
                Transform treeWorld;
                if (ReadTransform(ReadMemory, tree + 0x7c, treeWorld)) info.Root = {treeWorld.Position.x, treeWorld.Position.y, treeWorld.Position.z};
                TreeInfos.push_back(info);
            }
            std::vector<int> indices;
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto entry = data + i * 0x80;
                uintptr_t object{}, name{};
                int16_t parent{};
                std::string text;
                if (!Guard(entry + 0x70, object) || !Guard(entry + 0x78, name) || !Guard(entry + 0x68, parent) ||
                    !ReadMemory.Name(name, text) || parent < -1 || parent >= static_cast<int>(i)) return NativeFailure::Topology;
                int index = object ? Visit(object, -1, 0) : static_cast<int>(Nodes.size());
                if (index < 0 || index >= 1024) return NativeFailure::Topology;
                const int logicalParent = parent < 0 ? treeIndex : indices[parent];
                if (!object)
                {
                    Nodes.push_back({0, text, {}, entry, logicalParent});
                    Nodes.back().Identity = (entry ^ name) * 1099511628211ULL;
                }
                auto& node = Nodes[index];
                if (node.EntryIndex < 0) { node.EntryIndex = static_cast<int>(i); node.TreeSlot = static_cast<int>(treeSlot); node.EntryName = name; }
                node.Aliases.push_back({static_cast<int>(i), static_cast<int>(treeSlot), name});
                if (object && (!text.empty() && text != node.Name)) return NativeFailure::Topology;
                if (!flatAliases.insert(entry + 0x34).second || index == logicalParent) return NativeFailure::Topology;
                if (node.FlatParent != -2 && node.FlatParent != logicalParent) return NativeFailure::Topology;
                node.FlatParent = logicalParent;
                node.Worlds.push_back(entry + 0x34);
                node.Parent = logicalParent; // flattened parent graph is authoritative
                indices.push_back(index);
            }
        }
        std::unordered_map<uintptr_t, int> worlds;
        uint32_t mask{};
        for (size_t i = 0; i < Nodes.size(); ++i)
        {
            const auto& node = Nodes[i];
            const int semantic = Semantic(node.Name);
            if (semantic >= 0)
            {
                if (mask & (1u << semantic)) return NativeFailure::RequiredBody;
                mask |= 1u << semantic;
            }
            for (const auto address : node.Worlds) if (!worlds.emplace(address, static_cast<int>(i)).second) { Detail.Bone = node.Name; return SkinFailure("duplicate-world", -1, -1, node.Object, address); }
        }
        if ((mask & kRequiredMask) != kRequiredMask) return NativeFailure::RequiredBody;
        if (requireSkin) for (const auto index : Geometry)
        {
            uintptr_t skin{};
            if (!Guard(Nodes[index].Object + 0x170, skin))
            {
                Detail.Skin = {};
                Detail.Skin.Operand = "geometry-skin-read";
                Detail.Skin.Unreadable = true;
                SkinFailure("geometry-skin-read", index);
                return NativeFailure::Unreadable;
            }
            const bool valid = ReadSkinConsumers(skin,
                [&](uintptr_t address, auto& value) { return Guard(address, value); },
                [&](uintptr_t address, size_t bytes) { return ReadMemory.Range(address, bytes); },
                [&](uintptr_t bone, uintptr_t world) -> const char*
                {
                    const auto found = worlds.find(world);
                    if (found == worlds.end()) return "unmapped-world";
                    if (bone && Nodes[found->second].Object != bone)
                    {
                        Detail.Bone = Nodes[found->second].Name;
                        return "bone-alias-mismatch";
                    }
                    Nodes[found->second].Skin = true;
                    return nullptr;
                }, SkinStats, Detail.Skin);
            if (!valid)
            {
                SkinFailure(Detail.Skin.Operand, index, Detail.Skin.Slot, Detail.Skin.Bone, Detail.Skin.World);
                return Detail.Skin.Unreadable ? NativeFailure::Unreadable : NativeFailure::SkinAlias;
            }
        }
        if (requireSkin && !SkinStats.MappedSlots) return SkinFailure("no-skin-slots");
        Joints.resize(Nodes.size());
        for (size_t i = 0; i < Nodes.size(); ++i)
        {
            const auto& node = Nodes[i];
            auto& joint = Joints[i];
            joint.Parent = node.Parent;
            joint.Semantic = Semantic(node.Name);
            joint.RequiredForSkin = node.Skin;
            if (!ReadTransform(ReadMemory, node.Local, joint.Local) || !ReadTransform(ReadMemory, node.Worlds.front(), joint.World)) return NativeFailure::Unreadable;
        }
        return Stable() ? NativeFailure::None : NativeFailure::Changed;
    }
    // What Apply just wrote, in flattened-tree terms (the post-pass re-apply). One tree owns the snapshot: the
    // one with the most written entries. Written nodes outside that tree are published as items when their
    // native parent chain (Visit's child-array path, at most kMaxChain links) reaches one of its entry objects;
    // anything else is counted in Unmapped and stays a main-thread-only write. The tree's own world is never
    // republished: root motion stays the engine's.
    void BuildSnapshot(PostPass::Snapshot& out) const
    {
        out = {};
        if (TreeInfos.empty()) return;
        std::vector<uint32_t> written(TreeInfos.size());
        for (size_t i = 0; i < Joints.size(); ++i)
        {
            const auto& node = Nodes[i];
            if (Joints[i].Owned && !Joints[i].Pruned && node.EntryIndex >= 0 && node.TreeSlot >= 0) ++written[node.TreeSlot];
        }
        size_t slot = 0, populated = 0;
        for (size_t t = 0; t < written.size(); ++t) { if (written[t]) ++populated; if (written[t] > written[slot]) slot = t; }
        if (!written[slot]) return;
        out.MultiTree = populated > 1;
        const auto& info = TreeInfos[slot];
        out.Tree = Nodes[info.Node].Object;
        out.Data = info.Data;
        out.Count = info.Count;
        out.TreeCount = static_cast<uint32_t>(TreeInfos.size());
        out.RootPosition = info.Root;
        for (size_t i = 0; i < Joints.size(); ++i)
        {
            const auto& joint = Joints[i];
            if (!joint.Owned || joint.Pruned) continue;
            const auto& node = Nodes[i];
            const auto raw = Encode(joint.World);
            PostPass::RawTransform world;
            std::memcpy(world.Bytes.data(), &raw, sizeof(raw));
            if (node.EntryIndex >= 0)
            {
                // One EntryWrite per flattened alias of this node; the object world is written by the first only.
                // An entry whose object is the tree itself restores only its flattened world: tree+0x7c stays the engine's.
                bool objectWrite = node.Object != out.Tree;
                for (const auto& alias : node.Aliases)
                {
                    if (alias.TreeSlot != static_cast<int>(slot)) { ++out.Unmapped; continue; }
                    out.Entries.push_back({static_cast<uint32_t>(alias.Index), node.Object, alias.Name, objectWrite, world});
                    objectWrite = false;
                }
                continue;
            }
            if (!node.Object || node.Object == out.Tree || i == 0) continue; // the tree itself and the actor root are the engine's
            PostPass::ItemWrite item;
            std::array<uintptr_t, PostPass::kMaxChain> reversed{}, reversedTables{};
            uint32_t length = 0;
            bool reached = false;
            for (int p = node.Parent; p >= 0 && length < PostPass::kMaxChain; p = Nodes[p].Parent)
            {
                const auto& parent = Nodes[p];
                if (!parent.Object || !parent.Table) break;
                reversedTables[length] = parent.Table;
                reversed[length++] = parent.Object;
                if (parent.EntryIndex >= 0)
                {
                    reached = parent.TreeSlot == static_cast<int>(slot);
                    if (reached) item.EntryIndex = static_cast<uint32_t>(parent.EntryIndex);
                    break;
                }
            }
            if (!reached || out.Items.size() >= PostPass::kMaxItems) { ++out.Unmapped; continue; }
            item.ChainLength = length;
            for (uint32_t k = 0; k < length; ++k) { item.Chain[k] = reversed[length - 1 - k]; item.ChainVtables[k] = reversedTables[length - 1 - k]; }
            item.Object = node.Object;
            item.World = world;
            out.Items.push_back(item);
        }
    }
};

NativeBody::NativeBody() : m_impl(std::make_unique<Impl>()) {}
NativeBody::~NativeBody() = default;
uint64_t NativeBody::StableMicros() const { return m_impl->StableMicros; }
NativeReadStats NativeBody::Stats() const
{
    auto stats = m_impl->ReadMemory.Stats;
    stats.Nodes = static_cast<uint32_t>(m_impl->Nodes.size());
    stats.Guards = static_cast<uint32_t>(m_impl->Checks.size());
    stats.SkinInstances = m_impl->SkinStats.Instances;
    stats.EmptySkins = m_impl->SkinStats.Empty;
    stats.UnlinkedSkins = m_impl->SkinStats.Unlinked;
    stats.NullSkinSlots = m_impl->SkinStats.NullSlots;
    stats.MappedSkinSlots = m_impl->SkinStats.MappedSlots;
    return stats;
}
const NativeDiagnostic& NativeBody::Diagnostic() const { return m_impl->Detail; }

NativeFailure NativeBody::Read(NiNode* root, bool skin) { return m_impl->Read(root, skin); }
Transform NativeBody::RootWorld() const { return m_impl->Joints.empty() ? Transform{} : m_impl->Joints[0].World; }
uint64_t NativeBody::Content(bool source) const
{
    if (!source)
    {
        std::vector<uint64_t> identities;
        for (const auto& node : m_impl->Nodes)
        {
            auto identity = node.Identity;
            for (const auto world : node.Worlds) identity = (identity ^ world) * 1099511628211ULL;
            identities.push_back(identity);
        }
        return ReferenceContentIdentity(m_impl->Joints, identities, RootWorld().Scale);
    }
    // Transient gear/effect subtrees are not a new captured body generation.
    uint64_t hash = 1469598103934665603ULL;
    hash = (hash ^ std::bit_cast<uint32_t>(RootWorld().Scale)) * 1099511628211ULL;
    for (size_t i = 0; i < m_impl->Joints.size(); ++i)
    {
        const auto& joint = m_impl->Joints[i];
        if (joint.Semantic < 0 || !(kRequiredMask & (1u << joint.Semantic))) continue;
        hash = (hash ^ m_impl->Nodes[i].Worlds.front()) * 1099511628211ULL;
    }
    return hash;
}
Transform NativeBody::BoneWorld(int semantic) const
{
    for (const auto& joint : m_impl->Joints) if (joint.Semantic == semantic) return joint.World;
    return {{}, {NAN, NAN, NAN, NAN}, 0.f};
}
NativeFailure NativeBody::Bind(const ReferenceSkeleton& reference)
{
    auto& impl = *m_impl;
    std::vector<std::string_view> names;
    for (const auto& node : impl.Nodes) names.emplace_back(node.Name);
    // July's nonempty-but-partial skin cache was not success. Require every
    // finger/forearm-twist/toe in THIS reference, across all six groups, to be
    // consumed by a verified skin world alias before driving any of the body.
    std::vector<std::string_view> consumed;
    for (const auto& node : impl.Nodes) if (node.Skin) consumed.emplace_back(node.Name);
    if (!CompleteSkinDetails(reference, consumed, &impl.Detail.Bone)) return impl.SkinFailure("missing-skin-detail");
    if (BindReferenceLocals(impl.Joints, names, reference, &impl.Detail.Binding) != ReferenceFailure::None)
    {
        impl.Detail.Reason = "reference-binding";
        return NativeFailure::ReferenceBinding;
    }
    return NativeFailure::None;
}
bool NativeBody::Capture(const Transform& frame, Pose& pose) const
{
    if (!ValidTransform(frame)) return false;
    for (const auto& joint : m_impl->Joints)
    {
        if (joint.Semantic < 0) continue;
        auto measured = joint.World;
        // Per-bone scale is not transmitted or consumed by this pose path.
        // Only root scale normalizes positions; grips read their real hand
        // scale separately. Do not gate a usable rotation/position on an
        // unrelated per-bone visibility scale.
        measured.Scale = frame.Scale;
        if (!ValidTransform(measured))
        {
            if (kRequiredMask & (1u << joint.Semantic)) return false;
            continue;
        }
        const auto relative = RelativeTo(frame, measured);
        pose.Bones[joint.Semantic].Position = relative.Position;
        pose.Bones[joint.Semantic].Rotation = relative.Rotation;
        pose.Mask |= 1u << joint.Semantic;
    }
    bool stable{};
    { MicrosScope span{m_impl->StableMicros}; stable = m_impl->Stable(); }
    return stable && pose.HasBody();
}
bool NativeBody::ReadHmdWorld(PlayerCharacter* player, Transform& world)
{
#if TP_SKYRIMVR
    if (!player) return false;
    auto& memory = m_impl->ReadMemory;
    uintptr_t node{}, text{};
    std::string name;
    return memory.Read(reinterpret_cast<uintptr_t>(player) + 0x570, node) && node && memory.Kind(node, "NiAVObject") &&
        memory.Read(node + 0x10, text) && memory.Name(text, name) && name == "HmdNode" &&
        ReadTransform(memory, node + 0x7c, world) && ValidTransform(world);
#else
    return false;
#endif
}
void NativeBody::CaptureGrips(SourceSample& sample) const
{
    // Exact item/hand transforms come from the SAME already-read body tree.
    // Source draw state is explicit; attachment ancestry also excludes holsters.
    for (size_t h = 0; h < 2; ++h)
    {
        sample.GripState[h] = sample.Drawn ? GripCaptureState::NoMatchInScan : GripCaptureState::Sheathed;
        if (!sample.Drawn) continue;
        const auto selected = SelectAttachedItem(m_impl->Nodes, h);
        if (selected.Ambiguous || selected.InvalidTree) { sample.GripState[h] = GripCaptureState::RejectedScan; continue; }
        if (selected.Index < 0) continue;
        sample.LocalGripItems[h] = selected.Form;
        sample.GripState[h] = GripCaptureState::InvalidTransform;
        const auto hand = BoneWorld(h == 0 ? 17 : 25);
        const auto& item = m_impl->Joints[selected.Index].World;
        if (!ValidTransform(hand) || !ValidTransform(item)) continue;
        const auto grip = RelativeTo(hand, item);
        if (!ValidTransform(grip)) continue;
        sample.Body.Grips[h].Position = grip.Position;
        sample.Body.Grips[h].Rotation = grip.Rotation;
        sample.GripState[h] = GripCaptureState::Captured;
        sample.Body.GripMask |= 1u << h;
    }
    // Pure snapshot calculation: Capture already checked these guarded inputs.
}
NativeFailure NativeBody::Apply(const Pose& pose, const Transform& anchor, const std::array<uint32_t, 2>& items, float& handError, GripReport* gripReport, PostPass::Snapshot* snapshot)
{
    auto& impl = *m_impl;
    GripReport report{};
    uint8_t prepared{};
    std::array<int, 2> gripNodes{-1, -1};
    // Publish refusals even when the body fails later. Applied is set only
    // after the world writes commit, never for scratch composition alone.
    struct Publish { GripReport* Target; GripReport& Value; ~Publish() { if (Target) *Target = Value; } } publish{gripReport, report};
    std::array<int, 2> hands{-1, -1};
    for (size_t i = 0; i < impl.Joints.size(); ++i)
        for (int h = 0; h < 2; ++h) if (impl.Joints[i].Semantic == (h == 0 ? 17 : 25)) hands[h] = static_cast<int>(i);
    for (int h = 0; h < 2; ++h)
    {
        const uint8_t bit = 1u << h;
        if (!(pose.GripMask & bit)) continue;
        if (!items[h]) { report.Unmapped |= bit; continue; }
        if (hands[h] < 0) { report.Missing |= bit; continue; }
        const auto selected = SelectAttachedItem(impl.Nodes, h, items[h]);
        if (selected.InvalidTree) return NativeFailure::Topology;
        const int match = selected.Ambiguous ? -2 : selected.Index;
        if (match >= 0)
        {
            prepared |= bit;
            gripNodes[h] = match;
            impl.LastGrip[h] = match;
            auto& joint = impl.Joints[match];
            joint.Parent = hands[h];
            joint.Local.Position = pose.Grips[h].Position;
            joint.Local.Rotation = pose.Grips[h].Rotation;
            joint.Local.Scale = impl.Joints[match].World.Scale / impl.Joints[hands[h]].World.Scale;
            // Scratch reparent only. Native parent/local and bind data stay put.
        }
        else if (match == -2) report.Ambiguous |= bit;
        else report.Missing |= bit;
    }
    if (!ComposeBody(impl.Joints, anchor, pose)) return NativeFailure::Transform;
    handError = 0.f;
    for (const auto hand : hands)
        if (hand >= 0) handError = std::max(handError, glm::length(impl.Joints[hand].World.Position -
            (anchor.Position + anchor.Rotation * (pose.Bones[impl.Joints[hand].Semantic].Position * anchor.Scale))));
    for (size_t i = 0; i < impl.Joints.size(); ++i)
    {
        const auto& joint = impl.Joints[i];
        if (joint.RequiredForSkin && (!joint.Owned || joint.Pruned)) return NativeFailure::SkinAlias;
        if (!joint.Owned || joint.Pruned) continue;
        for (const auto address : impl.Nodes[i].Worlds) if (!impl.ReadMemory.Range(address, sizeof(NiTransformView), true)) return NativeFailure::Unreadable;
    }
    if (!impl.Stable()) return NativeFailure::Changed;
    // All transforms and destinations validated before the first write. Exact
    // aliases share a computed joint; each unique world address is written once.
    for (size_t i = 0; i < impl.Joints.size(); ++i)
    {
        const auto& joint = impl.Joints[i];
        if (!joint.Owned || joint.Pruned) continue;
        const auto raw = Encode(joint.World);
        for (const auto address : impl.Nodes[i].Worlds) std::memcpy(reinterpret_cast<void*>(address), &raw, sizeof(raw));
    }
    if (snapshot) impl.BuildSnapshot(*snapshot);
    for (size_t h = 0; h < gripNodes.size(); ++h)
        if (prepared & (1u << h))
        {
            const auto& joint = impl.Joints[gripNodes[h]];
            if (joint.Owned && !joint.Pruned) report.Applied |= 1u << h;
            else report.Pruned |= 1u << h;
        }
    return NativeFailure::None;
}

uint64_t BodySteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace
{
struct Mailbox
{
    std::mutex Mutex;
    std::atomic<bool> Enabled{}, Stopped{};
    SourceSample Sample;
    uint64_t Content{};
    uint32_t Generation{1};
};
// Deliberately never deleted. HIGGS has no remove callback; in-flight readers
// may outlive World and CRT teardown. There is no service pointer here.
Mailbox* const g_mailbox = new Mailbox;

#if TP_SKYRIMVR
bool TrackingAvailable(bool& lost)
{
    lost = false;
    const auto module = GetModuleHandleW(L"openvr_api.dll");
    if (!module) return false; // startup/temporary API absence is not positive loss
    using GetInterface = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
    const auto get = reinterpret_cast<GetInterface>(GetProcAddress(module, "VR_GetGenericInterface"));
    if (!get) return false;
    vr::EVRInitError error{};
    auto* system = static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error));
    if (!system || error != vr::VRInitError_None) return false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, poses, vr::k_unMaxTrackedDeviceCount);
    const std::array<vr::TrackedDeviceIndex_t, 3> devices{vr::k_unTrackedDeviceIndex_Hmd,
        system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand),
        system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand)};
    for (const auto device : devices)
    {
        if (device >= vr::k_unMaxTrackedDeviceCount || !poses[device].bDeviceIsConnected) { lost = true; return false; }
        // Optical hiccups are silence; disconnected/asleep devices are loss.
        if (!poses[device].bPoseIsValid) return false;
    }
    return true;
}
void CaptureCallback()
{
    auto& mailbox = *g_mailbox;
    if (!mailbox.Enabled.load(std::memory_order_acquire) || mailbox.Stopped.load(std::memory_order_acquire)) return;
    // Serialize any overlapping HIGGS invocation and shutdown with this copy.
    std::lock_guard lock(mailbox.Mutex);
    if (!mailbox.Enabled.load() || mailbox.Stopped.load()) return;
    SourceSample sample;
    const auto captureBegin = std::chrono::steady_clock::now();
    sample.CaptureThread = GetCurrentThreadId();
    sample.Serial = mailbox.Sample.Serial + 1;
    sample.SteadyMs = BodySteadyMs();
    const auto capture = [&]
    {
        if (!GetModuleHandleW(L"vrik.dll")) { sample.State = SourceState::Lost; sample.Failure = CaptureFailure::Provider; return; }
        bool lost{};
        if (!TrackingAvailable(lost))
        {
            sample.Failure = lost ? CaptureFailure::TrackingLost : CaptureFailure::TrackingUnavailable;
            if (lost) sample.State = SourceState::Lost;
            return;
        }
        auto* player = PlayerCharacter::Get();
        auto* root = player ? player->GetNiNode() : nullptr;
        if (!root) { sample.State = SourceState::Lost; sample.Failure = CaptureFailure::No3D; return; }
        NativeBody body;
        struct RecordStats
        {
            NativeBody& Body; SourceSample& Sample;
            ~RecordStats() { Sample.ReadStats = Body.Stats(); }
        } record{body, sample};
        NativeFailure read;
        { MicrosScope span{sample.ReadMicros}; read = body.Read(root, false); }
        if (read != NativeFailure::None) { sample.Failure = CaptureFailure::NativeRead; sample.NativeDetail = static_cast<uint32_t>(read); return; }
        {
            MicrosScope span{sample.PoseMicros};
            Transform hmdWorld;
            bool hmd{};
            { MicrosScope hmdSpan{sample.HmdMicros}; hmd = body.ReadHmdWorld(player, hmdWorld); }
            if (!hmd) { sample.Failure = CaptureFailure::HmdNode; return; }
            const auto frame = SourceMovementFrame({player->position.x, player->position.y, player->position.z}, hmdWorld.Position, body.RootWorld().Scale);
            const bool captured = body.Capture(frame, sample.Body);
            sample.StableMicros = body.StableMicros();
            if (!captured) { sample.Failure = CaptureFailure::RequiredPose; return; }
            if (body.Content(true) != mailbox.Content)
            {
                if (mailbox.Generation == UINT32_MAX) { sample.Failure = CaptureFailure::GenerationExhausted; return; }
                mailbox.Content = body.Content(true);
                ++mailbox.Generation;
            }
            sample.Body.Generation = mailbox.Generation;
        }
        {
            MicrosScope span{sample.GripMicros};
            sample.Drawn = player->actorState.IsWeaponDrawn();
            body.CaptureGrips(sample);
        }
        if (player->GetNiNode() != root) { sample.Failure = CaptureFailure::RootChanged; return; }
        sample.State = SourceState::Captured;
    };
    // No logs, World, ECS, transport, synchronized clock, or service lifetime.
    try { capture(); } catch (...) { sample.State = SourceState::TransientGap; sample.Failure = CaptureFailure::Exception; }
    sample.CaptureMicros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - captureBegin).count();
    mailbox.Sample = sample;
}
#endif
}
void RegisterBodyCaptureAtStartup()
{
#if TP_SKYRIMVR
    static bool attempted{};
    if (attempted) return;
    attempted = true;
    // Registered whatever the server later decides: HIGGS cannot take a callback after startup. Until the body
    // service enables the mailbox (body mode, connected) the callback returns after one atomic load.
    auto* higgs = HiggsAPI::Acquire();
    if (!higgs || higgs->GetBuildNumber() != 1101000 || !GetModuleHandleW(L"vrik.dll"))
    {
        spdlog::warn("BODY startup refused: requires HIGGS 1.10.10 and VRIK present at cold startup");
        return;
    }
    higgs->AddPostVrikPostHiggsCallback(&CaptureCallback);
    spdlog::info("BODY startup: post-VRIK capture registered once before GameWinMain; schema=2 world axes");
#endif
}
void EnableBodyCapture(bool enabled)
{
    std::lock_guard lock(g_mailbox->Mutex);
    g_mailbox->Enabled.store(enabled, std::memory_order_release);
    // Reconnect cannot replay a previous connection's mailbox body.
    g_mailbox->Sample.Body = {};
    g_mailbox->Sample.State = enabled ? SourceState::TransientGap : SourceState::Lost;
    g_mailbox->Sample.SteadyMs = BodySteadyMs();
    ++g_mailbox->Sample.Serial;
}
void StopBodyCapture()
{
    g_mailbox->Stopped.store(true, std::memory_order_release);
    EnableBodyCapture(false); // drains an invocation already inside the lock
}
void ReportBodyCaptureLoss()
{
    std::lock_guard lock(g_mailbox->Mutex);
    if (g_mailbox->Sample.State == SourceState::Lost) return;
    g_mailbox->Sample.Body = {};
    g_mailbox->Sample.State = SourceState::Lost;
    g_mailbox->Sample.Failure = CaptureFailure::No3D;
    g_mailbox->Sample.SteadyMs = BodySteadyMs();
    ++g_mailbox->Sample.Serial;
}
SourceSample ReadBodyCapture()
{
    std::lock_guard lock(g_mailbox->Mutex);
    return g_mailbox->Sample;
}
}
