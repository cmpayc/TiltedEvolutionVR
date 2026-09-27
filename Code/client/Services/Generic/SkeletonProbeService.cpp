#include <TiltedOnlinePCH.h>

#include <Services/SkeletonProbeService.h>
#include <Services/ImguiService.h>
#include <Services/DebugService.h>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Games/Memory.h>
#include <Games/TES.h>

#include <Misc/BSFixedString.h>
#include <NetImmerse/NiNode.h>
#include <PlayerCharacter.h>

#include <World.h>

#if TP_SKYRIMVR
#include <ModCompat/HiggsAPI.h>
#endif

#include <imgui.h>

namespace
{
/**
 * @brief NiAVObject transform layout, reused from the measurement documented in ObjectService.cpp.
 *
 * NiTransform is NiMatrix3 (0x24) + NiPoint3 (0xC) + float scale (0x4) = 0x34, and NiAVObject holds
 * three of them back to back: local, world, previousWorld. ObjectService established world.translate at
 * 0xA0 by scanning a held object's node for its known position, and found the three translates 0x34
 * apart at 0x6C / 0xA0 / 0xD4. Subtracting the 0x24 matrix from each gives the transform starts. The
 * offsets are the same on SE and VR, because VR's extra 0x28 bytes sit after these fields.
 */
constexpr size_t kLocalRotate = 0x48;
constexpr size_t kWorldRotate = 0x7C;
constexpr size_t kWorldTranslate = 0xA0;

// NiObjectNET::name, a BSFixedString. NiObject ends at 0x10 and NiAVObject's own fields start at 0x30,
// which is exactly the span NiObjectNET occupies in this client's headers (unk10[0x20]).
constexpr size_t kNodeName = 0x10;

// How far into PlayerCharacter the wand scan looks. The object is a little over 0xB00 bytes on VR, and
// overshooting is harmless because every candidate is validated before it is dereferenced.
constexpr size_t kScanBytes = 0xC00;

// Names worth looking for, most specific first. SkyrimVR's controller nodes are the point of the scan;
// the hand bones are the fallback that lets the probe do something useful on an SE build.
constexpr const char* kLeftCandidates[] = {"LeftWandNode", "NPC L Hand [LHnd]"};
constexpr const char* kRightCandidates[] = {"RightWandNode", "NPC R Hand [RHnd]"};

constexpr const char* kTargetBone[2] = {"NPC L Hand [LHnd]", "NPC R Hand [RHnd]"};
constexpr const char* kForearmBone[2] = {"NPC L Forearm [LLar]", "NPC R Forearm [RLar]"};
constexpr const char* kUpperArmBone[2] = {"NPC L UpperArm [LUar]", "NPC R UpperArm [RUar]"};

// Depth cap for the subtree walk. The deepest thing under a shoulder is a finger tip, well inside this.
constexpr uint32_t kMaxSubtreeDepth = 12;

/**
 * @brief BSFlattenedBoneTree's bone array, measured on SkyrimVR 1.4.15.
 *
 * The skinning reads bone transforms out of this array rather than off the bone NiNodes, which is why posing
 * the nodes gives a geometrically perfect skeleton the renderer ignores.
 *
 * Established by searching the array for the six arm bone node pointers, whose addresses were known exactly,
 * then reading one entry as raw floats against known reference values. Anchored on the refNode pointer at
 * entry+0x70:
 *
 *   entry+0x00  local NiTransform   (translate at +0x24 matched the node's local translate)
 *   entry+0x34  world NiTransform   (translate at +0x58 matched the node's world translate)
 *   entry+0x68  four int16 indices  (first was 0xFFFF, no parent)
 *   entry+0x70  NiAVObject* refNode
 *   entry+0x78  a second pointer
 *
 * The two 0x34 byte transforms plus the indices and the pointer tile exactly into the 0x80 stride, which is
 * what makes the layout unambiguous rather than a plausible guess. Validated again at runtime before anything
 * is written: see ResolveArmChains.
 */
constexpr size_t kTreeBoneArrayPtr = 0x158;
constexpr size_t kBoneEntryStride = 0x80;
constexpr size_t kBoneEntryLocal = 0x00;
constexpr size_t kBoneEntryWorld = 0x34;
constexpr size_t kBoneEntryRefNode = 0x70;

// The four int16s at entry+0x68. One of them is the parent's index in the array; which one is worked out at
// runtime by checking it against a hierarchy that is already known, rather than assumed here.
constexpr size_t kBoneEntryIndices = 0x68;
constexpr size_t kBoneIndexFields = 4;

// Sanity cap on the bone count when walking the array. A humanoid skeleton is well inside this.
constexpr size_t kMaxBones = 1024;

bool IsReadable(const void* apPtr, const size_t aSize) noexcept
{
    if (!apPtr)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apPtr, &info, sizeof(info)))
        return false;

    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cReadable))
        return false;

    const auto cRegionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;

    return reinterpret_cast<uintptr_t>(apPtr) + aSize <= cRegionEnd;
}

// Copies a C string only as far as the containing memory region reaches, so a bad candidate pointer in
// the scan cannot fault. Rejects anything non-printable, because every node name in the game is ascii.
bool SafeCopyString(const char* apPtr, char* apOut, const size_t aMax) noexcept
{
    if (!apPtr || aMax == 0)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apPtr, &info, sizeof(info)))
        return false;

    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cReadable))
        return false;

    const auto cRegionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;

    for (size_t i = 0; i + 1 < aMax; ++i)
    {
        if (reinterpret_cast<uintptr_t>(apPtr) + i >= cRegionEnd)
            return false;

        const char c = apPtr[i];
        apOut[i] = c;

        if (c == '\0')
            return true;

        if (c < 0x20 || c > 0x7E)
            return false;
    }

    return false;
}

const char* GetNodeName(const NiAVObject* apNode) noexcept
{
    if (!IsReadable(apNode, kNodeName + sizeof(const char*)))
        return nullptr;

    return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(apNode) + kNodeName);
}

/**
 * @brief Whether a candidate pointer can safely have a virtual call made on it.
 *
 * Readable memory is not enough. A virtual call also dereferences the vtable pointer stored inside that
 * memory and then calls through one of its slots, and the scan turns up plenty of values that point at
 * readable memory while being no kind of object at all. So the vtable is walked by hand first: the table
 * itself has to be readable, and the slot about to be called has to land in executable memory.
 *
 * Checking this rather than trusting the pointer is what the earlier version of this file got wrong.
 */
bool HasCallableVtable(const void* apObject) noexcept
{
    if (!IsReadable(apObject, sizeof(void*)))
        return false;

    const auto* pVtable = *reinterpret_cast<void* const* const*>(apObject);

    // Slot 0 is the destructor, slot 1 is GetRTTI. Both have to exist to call the latter.
    if (!IsReadable(pVtable, sizeof(void*) * 2))
        return false;

    const void* pGetRtti = pVtable[1];
    if (!pGetRtti)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pGetRtti, &info, sizeof(info)))
        return false;

    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    return (info.Protect & cExecutable) != 0;
}

// NiRTTI's first field is its name. The client models NiRTTI as an empty struct, so it is read directly.
// GetRTTI is virtual slot 1, below the slot where VR inserts its extra entry, so it dispatches correctly
// on both builds.
const char* GetRttiName(NiAVObject* apNode) noexcept
{
    if (!HasCallableVtable(apNode))
        return nullptr;

    const auto* pRtti = reinterpret_cast<const char* const*>(apNode->GetRTTI());
    if (!IsReadable(pRtti, sizeof(const char*)))
        return nullptr;

    return *pRtti;
}

// Every node type in the game ends in "Node" and derives from NiNode, so its children array sits at the
// same offset. Everything else (geometry, particle systems) does not, and must not be walked as one.
bool IsNodeType(NiAVObject* apNode) noexcept
{
    const char* pName = GetRttiName(apNode);
    if (!pName)
        return false;

    char buffer[64]{};
    if (!SafeCopyString(pName, buffer, sizeof(buffer)))
        return false;

    const size_t cLength = strlen(buffer);

    return cLength >= 4 && strcmp(buffer + cLength - 4, "Node") == 0;
}

glm::vec3 ReadWorld(const NiAVObject* apNode) noexcept
{
    const auto* pTranslate = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(apNode) + kWorldTranslate);

    return glm::vec3(pTranslate[0], pTranslate[1], pTranslate[2]);
}

// A bone's world transform, as the game stores it: a rotation whose columns are the bone's local axes, plus
// a translation. Scale is left alone.
struct Xform
{
    glm::mat3 Rotate{1.f};
    glm::vec3 Translate{};
};

Xform ReadXform(const NiAVObject* apNode) noexcept
{
    const auto* pBase = reinterpret_cast<const uint8_t*>(apNode);
    const auto* pR = reinterpret_cast<const float*>(pBase + kWorldRotate);
    const auto* pT = reinterpret_cast<const float*>(pBase + kWorldTranslate);

    Xform out{};

    // Stored row major as m[row][col]; glm::mat3 is column major, so indices swap.
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out.Rotate[col][row] = pR[row * 3 + col];

    out.Translate = glm::vec3(pT[0], pT[1], pT[2]);

    return out;
}

void WriteXform(NiAVObject* apNode, const Xform& acXform) noexcept
{
    auto* pBase = reinterpret_cast<uint8_t*>(apNode);
    auto* pR = reinterpret_cast<float*>(pBase + kWorldRotate);
    auto* pT = reinterpret_cast<float*>(pBase + kWorldTranslate);

    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            pR[row * 3 + col] = acXform.Rotate[col][row];

    pT[0] = acXform.Translate.x;
    pT[1] = acXform.Translate.y;
    pT[2] = acXform.Translate.z;
}

/**
 * @brief Poses one bone and carries everything below it by the same rigid motion.
 *
 * The delta from the bone's live world transform A to its wanted transform B is B * inverse(A), and for an
 * orthonormal rotation the inverse is just the transpose. Every descendant gets that delta applied to its own
 * world transform, which keeps the whole limb rigid below the joint.
 *
 * Called parent first. Each call reads the bone's *current* transform, so a bone already moved by its parent's
 * delta contributes correctly to its own, and the motions compose down the chain the way the hierarchy would
 * have done it.
 */
// Reads or writes a transform at an arbitrary base address, in the game's row major layout with the bone's
// local axes as columns.
Xform ReadXformAt(const uint8_t* acpBase) noexcept
{
    const auto* pR = reinterpret_cast<const float*>(acpBase);
    const auto* pT = reinterpret_cast<const float*>(acpBase + 0x24);

    Xform out{};

    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out.Rotate[col][row] = pR[row * 3 + col];

    out.Translate = glm::vec3(pT[0], pT[1], pT[2]);

    return out;
}

void WriteXformAt(uint8_t* apBase, const Xform& acXform) noexcept
{
    auto* pR = reinterpret_cast<float*>(apBase);
    auto* pT = reinterpret_cast<float*>(apBase + 0x24);

    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            pR[row * 3 + col] = acXform.Rotate[col][row];

    pT[0] = acXform.Translate.x;
    pT[1] = acXform.Translate.y;
    pT[2] = acXform.Translate.z;
}

// Composition, in the usual order: the result applies acSecond first, then acFirst.
Xform Compose(const Xform& acFirst, const Xform& acSecond) noexcept
{
    Xform out{};

    out.Rotate = acFirst.Rotate * acSecond.Rotate;
    out.Translate = acFirst.Rotate * acSecond.Translate + acFirst.Translate;

    return out;
}

/**
 * @brief Writes a bone's world transform, and the matching local transform, to the node and to its array slot.
 *
 * Writing world alone is not enough, and this is what several iterations of this probe got wrong. Anything that
 * recomputes world from local before the skin is read discards a world-only write, and VRIK's own internals say
 * as much: it works in local transforms and then propagates. So the local is updated to stay consistent with
 * the world being asked for, and both halves of both copies are written.
 *
 * The new local needs no parent transform. Since world = parent * local, and the parent is unchanged by this
 * write, local_new = local_old * (inverse(world_old) * world_new). Every term there is already to hand, which
 * avoids depending on a parent pointer or on the array's parent indices, neither of which is verified.
 */
// A bone's live world transform, from its node when it has one and from its array slot when it does not. Most
// bones under the hand have no node at all: the skeleton is flattened, so they exist only in the array.
Xform ReadPosed(const SkeletonProbeService::PosedNode& acTarget) noexcept
{
    if (acTarget.pNode)
        return ReadXformAt(reinterpret_cast<const uint8_t*>(acTarget.pNode) + kWorldRotate);

    if (acTarget.pFlatEntry)
        return ReadXformAt(acTarget.pFlatEntry + kBoneEntryWorld);

    return Xform{};
}

void WriteXformBoth(const SkeletonProbeService::PosedNode& acTarget, const Xform& acWanted, const bool aWriteFlat, const bool aUpdateLocal) noexcept
{
    // A bone with no node still has to be written, through its array slot alone.
    if (!acTarget.pNode)
    {
        if (aWriteFlat && acTarget.pFlatEntry)
            WriteXformAt(acTarget.pFlatEntry + kBoneEntryWorld, acWanted);

        return;
    }

    auto* pNodeBytes = reinterpret_cast<uint8_t*>(acTarget.pNode);

    Xform localNew{};
    bool haveLocal = false;

    if (aUpdateLocal)
    {
        const Xform cWorldOld = ReadXformAt(pNodeBytes + kWorldRotate);
        const Xform cLocalOld = ReadXformAt(pNodeBytes + kLocalRotate);

        // inverse(world_old) * world_new, using the transpose because the rotation is orthonormal.
        Xform delta{};
        delta.Rotate = glm::transpose(cWorldOld.Rotate) * acWanted.Rotate;
        delta.Translate = glm::transpose(cWorldOld.Rotate) * (acWanted.Translate - cWorldOld.Translate);

        localNew = Compose(cLocalOld, delta);
        haveLocal = true;

        WriteXformAt(pNodeBytes + kLocalRotate, localNew);
    }

    WriteXformAt(pNodeBytes + kWorldRotate, acWanted);

    if (!aWriteFlat || !acTarget.pFlatEntry)
        return;

    WriteXformAt(acTarget.pFlatEntry + kBoneEntryWorld, acWanted);

    if (haveLocal)
        WriteXformAt(acTarget.pFlatEntry + kBoneEntryLocal, localNew);
}

void PoseBoneAndSubtree(const SkeletonProbeService::PosedNode& acBone, const Xform& acWanted, const std::vector<SkeletonProbeService::PosedNode>& acSubtree, const bool aWriteFlat) noexcept
{
    if (!acBone.pNode)
        return;

    const Xform cCurrent = ReadPosed(acBone);

    const glm::mat3 cDeltaRotate = acWanted.Rotate * glm::transpose(cCurrent.Rotate);
    const glm::vec3 cDeltaTranslate = acWanted.Translate - cDeltaRotate * cCurrent.Translate;

    // The posed joint needs its local rebuilt, because its position relative to its parent genuinely changes.
    WriteXformBoth(acBone, acWanted, aWriteFlat, true);

    for (const SkeletonProbeService::PosedNode& cNode : acSubtree)
    {
        Xform node = ReadPosed(cNode);

        node.Rotate = cDeltaRotate * node.Rotate;
        node.Translate = cDeltaRotate * node.Translate + cDeltaTranslate;

        // Descendants move rigidly with the joint, and a rigid motion of a parent leaves a child's local
        // transform untouched by definition. Only their world transforms need writing.
        WriteXformBoth(cNode, node, aWriteFlat, false);
    }
}

void WriteWorld(NiAVObject* apNode, const glm::vec3& acPosition) noexcept
{
    auto* pTranslate = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(apNode) + kWorldTranslate);

    pTranslate[0] = acPosition.x;
    pTranslate[1] = acPosition.y;
    pTranslate[2] = acPosition.z;
}

/**
 * @brief The shortest rotation taking one direction onto another, by Rodrigues' formula.
 *
 * This is what replaces building a rotation from scratch. A bone's orientation is not just "along the bone":
 * Skyrim gives every bone a rest orientation and a roll about its own length, and inventing a frame from an
 * aim vector throws both away. That is why posed arms came out twisted, with the shoulder facing backwards and
 * the palm sitting wrong against the wrist.
 *
 * Rotating a bone from where it currently points to where it should point, and composing that onto its
 * existing rotation, changes the direction and preserves everything else. It also removes any need to know
 * which local axis a given bone treats as its length.
 */
glm::mat3 RotationBetween(const glm::vec3& acFrom, const glm::vec3& acTo) noexcept
{
    const float cLenFrom = glm::length(acFrom);
    const float cLenTo = glm::length(acTo);

    if (cLenFrom < 0.0001f || cLenTo < 0.0001f)
        return glm::mat3(1.f);

    const glm::vec3 cFrom = acFrom / cLenFrom;
    const glm::vec3 cTo = acTo / cLenTo;

    const float cDot = glm::clamp(glm::dot(cFrom, cTo), -1.f, 1.f);

    if (cDot > 0.99999f)
        return glm::mat3(1.f);

    glm::vec3 axis = glm::cross(cFrom, cTo);

    // Exactly opposed: the cross product vanishes and any perpendicular axis is a valid half turn.
    if (glm::length(axis) < 0.0001f)
    {
        const glm::vec3 cReference = std::abs(cFrom.x) < 0.9f ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 1.f, 0.f);
        axis = glm::cross(cFrom, cReference);
    }

    axis = glm::normalize(axis);

    const float cAngle = std::acos(cDot);
    const float cSin = std::sin(cAngle);
    const float cCos = std::cos(cAngle);

    const glm::mat3 cSkew(0.f, axis.z, -axis.y, -axis.z, 0.f, axis.x, axis.y, -axis.x, 0.f);

    return glm::mat3(1.f) + cSkew * cSin + (cSkew * cSkew) * (1.f - cCos);
}

/**
 * @brief A rotation whose local X axis runs down the bone.
 *
 * Kept only for the static test, where a bone's own orientation is not wanted. Not used for posing, because it
 * discards the rest roll: see RotationBetween.
 */
bool AimRotation(const glm::vec3& acAlong, const glm::vec3& acPlaneNormal, glm::mat3& aOut) noexcept
{
    const float cLength = glm::length(acAlong);
    if (cLength < 0.0001f)
        return false;

    const glm::vec3 cX = acAlong / cLength;

    glm::vec3 z = acPlaneNormal - cX * glm::dot(acPlaneNormal, cX);
    if (glm::length(z) < 0.0001f)
        return false;

    z = glm::normalize(z);

    const glm::vec3 cY = glm::cross(z, cX);

    // Columns are the local axes.
    aOut = glm::mat3(cX, cY, z);

    return true;
}

NiAVObject* FindByName(NiAVObject* apRoot, const char* acpName) noexcept
{
    if (!apRoot || !acpName)
        return nullptr;

    using TGetObjectByName = NiAVObject*(NiAVObject*, const char**, char);
    POINTER_SKYRIMSE(TGetObjectByName, GetObjectByName, 76207);

    BSFixedString name(acpName);
    if (!name.data)
        return nullptr;

    return GetObjectByName(apRoot, &name.data, 1);
}

// Flattens every descendant of a bone into a list, once, so the per frame path is pure arithmetic. The safety
// checks here cost a VirtualQuery per node and a virtual call for the type test, which is fine at resolve time
// and would not be at 90 frames a second.
//
// Recursion is not gated on the RTTI name ending in "Node". Doing that is what made an earlier version of this
// walk report an actor as having no arm bones at all, because BSFlattenedBoneTree sits above them and does not
// match.
void CollectSubtree(NiAVObject* apNode, const uint32_t aDepth, std::vector<NiAVObject*>& aOut) noexcept
{
    if (!IsReadable(apNode, sizeof(NiNode)) || aDepth > kMaxSubtreeDepth)
        return;

    auto* pAsNode = static_cast<NiNode*>(apNode);
    if (!IsReadable(pAsNode->children.data, sizeof(void*) * pAsNode->children.length))
        return;

    for (uint16_t i = 0; i < pAsNode->children.length; ++i)
    {
        NiAVObject* pChild = pAsNode->children[i];

        if (!IsReadable(pChild, kNiAVObjectSize))
            continue;

        aOut.push_back(pChild);
        CollectSubtree(pChild, aDepth + 1, aOut);
    }
}

// The start of BSFlattenedBoneTree's bone array, or null. Only the pointer's location is assumed here; every
// use of the entries is validated by the caller against live transforms.
uint8_t* GetBoneArray(NiAVObject* apTree) noexcept
{
    if (!IsReadable(apTree, kTreeBoneArrayPtr + sizeof(void*)))
        return nullptr;

    auto* pBlock = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(apTree) + kTreeBoneArrayPtr);

    return IsReadable(pBlock, kBoneEntryStride) ? pBlock : nullptr;
}

/**
 * @brief How many entries the bone array actually holds.
 *
 * An earlier version of this counted how many entries were *readable*, which is not the same thing at all:
 * readable memory runs on past the end of the heap block, so it reported up to the cap and the caller then
 * treated unrelated heap as bone entries. Entries whose garbage parent indices happened to chain back to a
 * real bone were written to, which corrupted the heap and crashed the game inside its own code.
 *
 * So the count is read from the tree and then proved against the array: every entry below it must be readable
 * and carry a refNode that is either null or a readable object. A count that fails is not used, and without a
 * count nothing is collected from the array.
 */
size_t FindBoneCount(const NiAVObject* acpTree, uint8_t* apBlock) noexcept
{
    if (!acpTree || !apBlock)
        return 0;

    const auto* pTreeBytes = reinterpret_cast<const uint8_t*>(acpTree);

    for (size_t offset = sizeof(NiNode); offset + sizeof(uint32_t) <= kTreeBoneArrayPtr + 0x20; offset += sizeof(uint32_t))
    {
        if (!IsReadable(pTreeBytes + offset, sizeof(uint32_t)))
            continue;

        const uint32_t cCandidate = *reinterpret_cast<const uint32_t*>(pTreeBytes + offset);

        if (cCandidate == 0 || cCandidate > kMaxBones)
            continue;

        bool plausible = true;

        for (uint32_t i = 0; i < cCandidate && plausible; ++i)
        {
            uint8_t* pEntry = apBlock + static_cast<size_t>(i) * kBoneEntryStride;

            if (!IsReadable(pEntry, kBoneEntryStride))
            {
                plausible = false;
                break;
            }

            auto* pRefNode = *reinterpret_cast<NiAVObject* const*>(pEntry + kBoneEntryRefNode);

            if (pRefNode && !IsReadable(pRefNode, kNiAVObjectSize))
                plausible = false;
        }

        if (plausible)
            return cCandidate;
    }

    return 0;
}

int16_t ReadBoneIndex(const uint8_t* acpEntry, const size_t aField) noexcept
{
    return *reinterpret_cast<const int16_t*>(acpEntry + kBoneEntryIndices + aField * sizeof(int16_t));
}

/**
 * @brief Works out which of the four int16s at entry+0x68 is the parent index.
 *
 * The arm hierarchy is already known: the forearm's parent is the upper arm, and the hand's parent is the
 * forearm. So the correct field is the one that, read as an array index, points from each bone to its known
 * parent. Nothing is assumed about which slot it is or what the others mean.
 *
 * Returns the field number, or -1 when no field fits, in which case descendants are not collected from the
 * array at all rather than being collected wrongly.
 */
int FindParentIndexField(uint8_t* apBlock, const size_t acCount, const uint8_t* acpUpper, const uint8_t* acpFore, const uint8_t* acpHand) noexcept
{
    if (!apBlock || !acpUpper || !acpFore || !acpHand)
        return -1;

    const auto cIndexOf = [&](const uint8_t* acpEntry) { return static_cast<int64_t>((acpEntry - apBlock) / kBoneEntryStride); };

    const int64_t cUpper = cIndexOf(acpUpper);
    const int64_t cFore = cIndexOf(acpFore);

    for (size_t field = 0; field < kBoneIndexFields; ++field)
    {
        const int16_t cForeParent = ReadBoneIndex(acpFore, field);
        const int16_t cHandParent = ReadBoneIndex(acpHand, field);

        if (cForeParent == cUpper && cHandParent == cFore && cUpper >= 0 && cFore >= 0 && static_cast<size_t>(cUpper) < acCount)
            return static_cast<int>(field);
    }

    return -1;
}

// Walks the bone array and returns the entry whose refNode is the given node, or null.
uint8_t* FindBoneEntry(uint8_t* apBlock, const NiAVObject* acpNode) noexcept
{
    if (!apBlock || !acpNode)
        return nullptr;

    for (size_t i = 0; i < kMaxBones; ++i)
    {
        uint8_t* pEntry = apBlock + i * kBoneEntryStride;

        if (!IsReadable(pEntry, kBoneEntryStride))
            return nullptr;

        if (*reinterpret_cast<const NiAVObject* const*>(pEntry + kBoneEntryRefNode) == acpNode)
            return pEntry;
    }

    return nullptr;
}

// Depth first search for a node by its RTTI type name. Used to find BSFlattenedBoneTree, which is where the
// skeleton hangs off and cannot be found by node name.
NiAVObject* FindByRttiName(NiAVObject* apNode, const char* acpWanted, const uint32_t aDepth) noexcept
{
    if (!IsReadable(apNode, kNiAVObjectSize) || aDepth > kMaxSubtreeDepth + 8)
        return nullptr;

    char rtti[64]{};
    if (SafeCopyString(GetRttiName(apNode), rtti, sizeof(rtti)) && strcmp(rtti, acpWanted) == 0)
        return apNode;

    if (!IsReadable(apNode, sizeof(NiNode)))
        return nullptr;

    auto* pAsNode = static_cast<NiNode*>(apNode);
    if (!IsReadable(pAsNode->children.data, sizeof(void*) * pAsNode->children.length))
        return nullptr;

    for (uint16_t i = 0; i < pAsNode->children.length; ++i)
    {
        if (NiAVObject* pFound = FindByRttiName(pAsNode->children[i], acpWanted, aDepth + 1))
            return pFound;
    }

    return nullptr;
}

void DumpTree(NiAVObject* apNode, const uint32_t aDepth, uint32_t& aCount) noexcept
{
    // Guarded the same way the scan is, because a corrupt or partially torn down node in a real children
    // array would otherwise fault exactly as an unvalidated scan candidate does.
    if (!IsReadable(apNode, kNiAVObjectSize) || aDepth > 12 || aCount > 4000)
        return;

    char name[128]{};
    if (!SafeCopyString(GetNodeName(apNode), name, sizeof(name)))
        strcpy_s(name, "<unnamed>");

    char rtti[64]{};
    if (!SafeCopyString(GetRttiName(apNode), rtti, sizeof(rtti)))
        strcpy_s(rtti, "<no rtti>");

    const glm::vec3 cWorld = ReadWorld(apNode);

    spdlog::info("{:>{}}{} [{}] ({:.1f}, {:.1f}, {:.1f})", "", aDepth * 2, name, rtti, cWorld.x, cWorld.y, cWorld.z);

    ++aCount;

    if (!IsNodeType(apNode))
        return;

    // The children array lives past the end of NiAVObject, so the object has to actually be that big
    // before its length can be read.
    if (!IsReadable(apNode, sizeof(NiNode)))
        return;

    auto* pAsNode = static_cast<NiNode*>(apNode);
    if (!IsReadable(pAsNode->children.data, sizeof(void*) * pAsNode->children.length))
        return;

    for (uint16_t i = 0; i < pAsNode->children.length; ++i)
        DumpTree(pAsNode->children[i], aDepth + 1, aCount);
}
} // namespace

SkeletonProbeService::SkeletonProbeService(entt::dispatcher& aDispatcher, World& aWorld, ImguiService& aImguiService)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&SkeletonProbeService::OnUpdate>(this);
    m_drawConnection = aImguiService.OnDraw.connect<&SkeletonProbeService::OnDraw>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&SkeletonProbeService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&SkeletonProbeService::OnDisconnected>(this);

    m_sourceName[0] = kLeftCandidates[1];
    m_sourceName[1] = kRightCandidates[1];

    // Nothing picked yet, so any candidate beats this.
    m_sourcePriority[0] = std::numeric_limits<int>::max();
    m_sourcePriority[1] = std::numeric_limits<int>::max();
}

void SkeletonProbeService::DumpPlayerTree() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->GetNiNode())
    {
        spdlog::warn("Skeleton probe: player has no 3D");
        return;
    }

    uint32_t count = 0;
    spdlog::info("--- Skeleton probe: player 3D tree ---");
    DumpTree(pPlayer->GetNiNode(), 0, count);
    spdlog::info("--- {} nodes ---", count);
}

void SkeletonProbeService::DumpTargetTree() noexcept
{
    auto* pTarget = Cast<Actor>(TESForm::GetById(m_targetFormId));
    if (!pTarget || !pTarget->GetNiNode())
    {
        spdlog::warn("Skeleton probe: target {:X} has no 3D", m_targetFormId);
        return;
    }

    uint32_t count = 0;
    spdlog::info("--- Skeleton probe: target {:X} 3D tree ---", m_targetFormId);
    DumpTree(pTarget->GetNiNode(), 0, count);
    spdlog::info("--- {} nodes ---", count);
}

/**
 * @brief Finds the VR controller nodes by scanning PlayerCharacter for a pointer to a node with a
 * matching name, and reports the offset.
 *
 * On VR the wand nodes are not children of the actor's 3D, so a name search from GetNiNode() does not
 * reach them. They are held in PlayerCharacter itself. Rather than hardcode an offset taken from another
 * project, this finds it here and logs it, so the number that ends up in the real feature is one this
 * build actually observed. Every candidate is validated before it is dereferenced.
 */
void SkeletonProbeService::ScanPlayerForWandNodes() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    spdlog::info("--- Skeleton probe: scanning PlayerCharacter at {} for node pointers ---", static_cast<void*>(pPlayer));

    const auto* pBase = reinterpret_cast<const uint8_t*>(pPlayer);
    uint32_t found = 0;

    for (size_t offset = 0; offset + sizeof(void*) <= kScanBytes; offset += sizeof(void*))
    {
        if (!IsReadable(pBase + offset, sizeof(void*)))
            continue;

        auto* pCandidate = *reinterpret_cast<NiAVObject* const*>(pBase + offset);

        // Filters in increasing cost, cheapest first. The object has to be big enough to be a node, and
        // its vtable has to be callable, which is checked by walking the table rather than by calling
        // through it. Only then is a virtual call safe.
        if (!IsReadable(pCandidate, kNiAVObjectSize) || !HasCallableVtable(pCandidate))
            continue;

        // The name is a plain field read at a fixed offset, no dispatch involved, and it is the thing the
        // scan is actually looking for. A candidate with a printable name of a sensible length is a node
        // for our purposes; the RTTI name below is logged for context only.
        char name[128]{};
        if (!SafeCopyString(GetNodeName(pCandidate), name, sizeof(name)))
            continue;

        if (strlen(name) < 2)
            continue;

        char rtti[64]{};
        if (!SafeCopyString(GetRttiName(pCandidate), rtti, sizeof(rtti)))
            strcpy_s(rtti, "<no rtti>");

        const glm::vec3 cWorld = ReadWorld(pCandidate);

        spdlog::info("  +0x{:03X} -> \"{}\" [{}] at ({:.1f}, {:.1f}, {:.1f})", offset, name, rtti, cWorld.x, cWorld.y, cWorld.z);
        ++found;

        // Candidate order is preference order, and a later offset must not beat an earlier, more wanted
        // name. Both the wand node and the player's own hand bone are cached in PlayerCharacter, the hand
        // bone at the higher offset, so keeping the last match picked the fallback over the real source.
        for (int i = 0; i < static_cast<int>(std::size(kLeftCandidates)); ++i)
        {
            if (strcmp(name, kLeftCandidates[i]) == 0 && i < m_sourcePriority[0])
            {
                m_sourceName[0] = name;
                m_sourceOffset[0] = offset;
                m_sourcePriority[0] = i;
                spdlog::info("    ^ left source set to \"{}\" at PlayerCharacter+0x{:03X}", name, offset);
            }
        }

        for (int i = 0; i < static_cast<int>(std::size(kRightCandidates)); ++i)
        {
            if (strcmp(name, kRightCandidates[i]) == 0 && i < m_sourcePriority[1])
            {
                m_sourceName[1] = name;
                m_sourceOffset[1] = offset;
                m_sourcePriority[1] = i;
                spdlog::info("    ^ right source set to \"{}\" at PlayerCharacter+0x{:03X}", name, offset);
            }
        }
    }

    spdlog::info("--- {} node pointers found in PlayerCharacter ---", found);
}

NiAVObject* SkeletonProbeService::ResolveSource(const size_t aHand) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return nullptr;

    // The scanned offset first. The VR controller nodes hang off PlayerCharacter and are not part of the
    // actor's 3D, so for them the name search below finds nothing. Re-read every call rather than cached,
    // because the game swaps these nodes.
    if (m_sourceOffset[aHand])
    {
        const auto* pSlot = reinterpret_cast<const uint8_t*>(pPlayer) + m_sourceOffset[aHand];

        if (IsReadable(pSlot, sizeof(void*)))
        {
            auto* pNode = *reinterpret_cast<NiAVObject* const*>(pSlot);

            if (IsReadable(pNode, kNiAVObjectSize))
                return pNode;
        }
    }

    NiNode* pRoot = pPlayer->GetNiNode();
    if (!pRoot || m_sourceName[aHand].empty())
        return nullptr;

    return FindByName(pRoot, m_sourceName[aHand].c_str());
}

void SkeletonProbeService::ResolveArmChains(NiNode* apTargetRoot) noexcept
{
    const bool cFirstResolve = m_lastSubtreeCount[0] == 0 && m_lastSubtreeCount[1] == 0;

    m_chain[0] = ArmChain{};
    m_chain[1] = ArmChain{};

    if (!apTargetRoot)
        return;

    m_flatLayoutConfirmed = false;

    // The array the skinning actually renders from. Everything below pairs each bone with its slot in it.
    NiAVObject* pTree = FindByRttiName(apTargetRoot, "BSFlattenedBoneTree", 0);
    uint8_t* pBoneArray = pTree ? GetBoneArray(pTree) : nullptr;

    if (cFirstResolve && !pBoneArray)
        spdlog::warn("PROBE: no BSFlattenedBoneTree bone array under {:X}. Falling back to posing the nodes only, which is known not to reach the screen.", m_targetFormId);

    for (size_t hand = 0; hand < 2; ++hand)
    {
        ArmChain& chain = m_chain[hand];

        chain.UpperArm.pNode = FindByName(apTargetRoot, kUpperArmBone[hand]);
        chain.Forearm.pNode = FindByName(apTargetRoot, kForearmBone[hand]);
        chain.Hand.pNode = FindByName(apTargetRoot, kTargetBone[hand]);

        std::vector<NiAVObject*> upper, fore, palm;
        CollectSubtree(chain.UpperArm.pNode, 0, upper);
        CollectSubtree(chain.Forearm.pNode, 0, fore);
        CollectSubtree(chain.Hand.pNode, 0, palm);

        for (NiAVObject* pNode : upper)
            chain.UpperSubtree.push_back(PosedNode{pNode, nullptr});
        for (NiAVObject* pNode : fore)
            chain.ForeSubtree.push_back(PosedNode{pNode, nullptr});
        for (NiAVObject* pNode : palm)
            chain.HandSubtree.push_back(PosedNode{pNode, nullptr});

        // Pair every bone with its slot in the array the skin renders from.
        PosedNode* all[3] = {&chain.UpperArm, &chain.Forearm, &chain.Hand};
        for (PosedNode* pTarget : all)
            pTarget->pFlatEntry = FindBoneEntry(pBoneArray, pTarget->pNode);

        for (std::vector<PosedNode>* pList : {&chain.UpperSubtree, &chain.ForeSubtree, &chain.HandSubtree})
        {
            for (PosedNode& target : *pList)
                target.pFlatEntry = FindBoneEntry(pBoneArray, target.pNode);
        }

        /**
         * Descendants that exist only in the array. The skeleton is flattened, so a bone such as a finger has
         * an entry with a parent index but no NiNode in the child tree at all. Collecting descendants by
         * walking NiNode children therefore misses every one of them, which is why a posed palm moved while
         * the fingers stayed behind and stretched.
         */
        const size_t cBoneCount = FindBoneCount(pTree, pBoneArray);
        const int cParentField = cBoneCount ? FindParentIndexField(pBoneArray, cBoneCount, chain.UpperArm.pFlatEntry, chain.Forearm.pFlatEntry, chain.Hand.pFlatEntry) : -1;

        if (cFirstResolve && hand == 0)
            spdlog::info("PROBE RESULT 10: bone array holds {} entries, parent index is field {}. A count of zero means the array is not trusted and nothing beyond the node tree is touched.", cBoneCount, cParentField);

        if (cParentField >= 0)
        {
            const auto cEntryAt = [&](const size_t aIndex) { return pBoneArray + aIndex * kBoneEntryStride; };

            // Which of the three joints, if any, each entry ultimately hangs off. Walking the parent chain per
            // entry is cheap at resolve time and needs no ordering assumptions about the array.
            for (size_t i = 0; i < cBoneCount; ++i)
            {
                uint8_t* pEntry = cEntryAt(i);

                if (pEntry == chain.UpperArm.pFlatEntry || pEntry == chain.Forearm.pFlatEntry || pEntry == chain.Hand.pFlatEntry)
                    continue;

                std::vector<PosedNode>* pOwner = nullptr;
                int16_t walk = static_cast<int16_t>(i);

                for (size_t step = 0; step < 64; ++step)
                {
                    walk = ReadBoneIndex(cEntryAt(static_cast<size_t>(walk)), static_cast<size_t>(cParentField));

                    if (walk < 0 || static_cast<size_t>(walk) >= cBoneCount)
                        break;

                    uint8_t* pAncestor = cEntryAt(static_cast<size_t>(walk));

                    if (pAncestor == chain.Hand.pFlatEntry)
                    {
                        pOwner = &chain.HandSubtree;
                        break;
                    }
                    if (pAncestor == chain.Forearm.pFlatEntry)
                    {
                        pOwner = &chain.ForeSubtree;
                        break;
                    }
                    if (pAncestor == chain.UpperArm.pFlatEntry)
                    {
                        pOwner = &chain.UpperSubtree;
                        break;
                    }
                }

                if (!pOwner)
                    continue;

                auto* pRefNode = *reinterpret_cast<NiAVObject* const*>(pEntry + kBoneEntryRefNode);

                // Skip the ones already collected from the node tree, so nothing is posed twice.
                const bool cAlready = std::any_of(pOwner->begin(), pOwner->end(), [&](const PosedNode& acExisting) { return acExisting.pFlatEntry == pEntry; });

                if (!cAlready)
                    pOwner->push_back(PosedNode{IsReadable(pRefNode, kNiAVObjectSize) ? pRefNode : nullptr, pEntry});
            }

            // A bone hangs off the elbow as well as the shoulder, and off the wrist as well as both, so the
            // outer lists have to contain the inner ones for the rigid carry to stay correct.
            for (const PosedNode& cNode : chain.HandSubtree)
            {
                if (std::none_of(chain.ForeSubtree.begin(), chain.ForeSubtree.end(), [&](const PosedNode& acN) { return acN.pFlatEntry == cNode.pFlatEntry; }))
                    chain.ForeSubtree.push_back(cNode);
            }
            for (const PosedNode& cNode : chain.ForeSubtree)
            {
                if (std::none_of(chain.UpperSubtree.begin(), chain.UpperSubtree.end(), [&](const PosedNode& acN) { return acN.pFlatEntry == cNode.pFlatEntry; }))
                    chain.UpperSubtree.push_back(cNode);
            }
        }
        else if (cFirstResolve)
        {
            spdlog::error("PROBE RESULT 9: could not identify the parent index field in the bone array, so bones that exist only there (fingers) cannot be found. They will stay behind.");
        }

        // Only report when something changed, because this now runs on a timer rather than once.
        if (chain.UpperSubtree.size() == m_lastSubtreeCount[hand])
            continue;

        m_lastSubtreeCount[hand] = chain.UpperSubtree.size();

        size_t paired = 0;
        for (const PosedNode& cNode : chain.UpperSubtree)
            paired += cNode.pFlatEntry ? 1 : 0;

        size_t entryOnly = 0;
        for (const PosedNode& cNode : chain.UpperSubtree)
            entryOnly += cNode.pNode ? 0 : 1;

        spdlog::info("PROBE RESULT 9 ({}): arm chain on {:X}: UpperArm {}, Forearm {}, Hand {}. {} bones below the shoulder, {} of which exist only in the flattened array and have no node ({} below the wrist).", hand == 0 ? "left" : "right", m_targetFormId, chain.UpperArm.pNode ? (chain.UpperArm.pFlatEntry ? "ok+slot" : "ok, NO SLOT") : "MISSING", chain.Forearm.pNode ? (chain.Forearm.pFlatEntry ? "ok+slot" : "ok, NO SLOT") : "MISSING", chain.Hand.pNode ? (chain.Hand.pFlatEntry ? "ok+slot" : "ok, NO SLOT") : "MISSING", chain.UpperSubtree.size(), entryOnly, chain.HandSubtree.size());
    }

    // Capture the reference orientation once per target, relative to the actor's root so it follows the actor
    // turning. Re-capturing on every resolve would feed the animation's roll straight back in.
    if (!m_restCaptured && m_chain[0].HasCore() && m_chain[1].HasCore())
    {
        const glm::mat3 cRootInverse = glm::transpose(ReadXform(apTargetRoot).Rotate);

        for (size_t hand = 0; hand < 2; ++hand)
        {
            const Xform cUpper = ReadPosed(m_chain[hand].UpperArm);
            const Xform cFore = ReadPosed(m_chain[hand].Forearm);
            const glm::vec3 cWrist = ReadPosed(m_chain[hand].Hand).Translate;

            m_restRotate[hand][0] = cRootInverse * cUpper.Rotate;
            m_restRotate[hand][1] = cRootInverse * cFore.Rotate;
            m_restDir[hand][0] = cRootInverse * (cFore.Translate - cUpper.Translate);
            m_restDir[hand][1] = cRootInverse * (cWrist - cFore.Translate);
        }

        m_restCaptured = true;

        spdlog::info("PROBE: captured a reference arm orientation for {:X}. The posed roll now comes from this rather than from whatever the walk animation is doing.", m_targetFormId);
    }

    // Validate the layout against live data before writing a single byte through it. For each bone that has a
    // slot, the world translate stored in the slot must already agree with the node's own world translate,
    // because nothing has written to either yet this frame. If they agree the offsets are right; if they do not,
    // the offsets are wrong and writing through them would quietly corrupt a skeleton.
    if (!pBoneArray)
        return;

    size_t checked = 0;
    size_t agreed = 0;
    float worst = 0.f;

    for (const ArmChain& cChain : m_chain)
    {
        for (const PosedNode* pTarget : {&cChain.UpperArm, &cChain.Forearm, &cChain.Hand})
        {
            if (!pTarget->pNode || !pTarget->pFlatEntry)
                continue;

            ++checked;

            const auto* pStored = reinterpret_cast<const float*>(pTarget->pFlatEntry + kBoneEntryWorld + 0x24);
            const glm::vec3 cStored(pStored[0], pStored[1], pStored[2]);
            const float cError = glm::length(cStored - ReadWorld(pTarget->pNode));

            worst = std::max(worst, cError);

            if (cError < 0.05f)
                ++agreed;
        }
    }

    m_flatLayoutConfirmed = checked > 0 && agreed == checked;

    if (cFirstResolve || !m_flatLayoutConfirmed)
    {
        if (m_flatLayoutConfirmed)
            spdlog::info("PROBE RESULT 5: flattened array layout confirmed on live data. {} of {} bone slots agree with their node's world translate (worst {:.4f}). Writing through the array is enabled.", agreed, checked, worst);
        else
            spdlog::error("PROBE RESULT 5: flattened array layout NOT confirmed. Only {} of {} bone slots agree (worst error {:.2f}). Array writes stay disabled rather than risk corrupting a skeleton.", agreed, checked, worst);
    }
}

void SkeletonProbeService::PickNearestTarget() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    ProcessLists* pProcessLists = ProcessLists::Get();

    if (!pPlayer || !pProcessLists)
        return;

    float best = std::numeric_limits<float>::max();
    uint32_t bestId = 0;

    uint32_t skippedNoArms = 0;

    for (uint32_t i = 0; i < pProcessLists->highActorHandleArray.length; ++i)
    {
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(pProcessLists->highActorHandleArray[i]));
        if (!pActor || pActor == pPlayer || !pActor->GetNiNode())
            continue;

        // The nearest actor in an exterior is usually a chicken, a cow or a horse, and none of those
        // skeletons have the humanoid arm bones. Driving one would write nothing and read as a failure of
        // the write path rather than of the target choice, so the bone has to exist to qualify.
        if (!FindByName(pActor->GetNiNode(), kTargetBone[0]))
        {
            ++skippedNoArms;
            continue;
        }

        const float cDistance = glm::length(glm::vec3(pActor->position) - glm::vec3(pPlayer->position));
        if (cDistance < best)
        {
            best = cDistance;
            bestId = pActor->formID;
        }
    }

    if (bestId)
    {
        m_targetFormId = bestId;
        spdlog::info("PROBE: target set to {:X} at {:.0f} units ({} nearby actors skipped for having no arm bones)", bestId, best, skippedNoArms);
    }
    else
    {
        spdlog::warn("PROBE: no loaded actor with arm bones to target ({} skipped for having no arms). Stand near a humanoid NPC.", skippedNoArms);
    }
}

// Reads back what the previous tick wrote. A drift above roughly a millimetre means something between
// the write and now rewrote the bone, which is the answer the probe exists to produce.
void SkeletonProbeService::CheckDrift() noexcept
{
    for (size_t hand = 0; hand < 2; ++hand)
    {
        if (!m_hasWritten[hand] || !m_target[hand].Resolved)
            continue;

        // The pointer was resolved a frame ago and the actor's 3D can be freed by a cell load in
        // between, so it is validated rather than trusted.
        if (!IsReadable(m_target[hand].pNode, kNiAVObjectSize))
        {
            m_hasWritten[hand] = false;
            continue;
        }

        m_readBack[hand] = ReadWorld(m_target[hand].pNode);
        m_drift[hand] = glm::length(m_readBack[hand] - m_written[hand]);

        if (m_drift[hand] > m_worstDrift)
            m_worstDrift = m_drift[hand];

        if (m_drift[hand] > 0.1f)
            ++m_overwritten;
    }
}

void SkeletonProbeService::Apply() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    NiNode* pPlayerRoot = pPlayer->GetNiNode();
    if (!pPlayerRoot)
        return;

    auto* pTarget = Cast<Actor>(TESForm::GetById(m_targetFormId));
    NiNode* pTargetRoot = pTarget ? pTarget->GetNiNode() : nullptr;

    // Not resolved per frame, because that would mean six BSFixedString constructions and a full subtree walk
    // every frame. Not resolved only once either: an actor's skeleton is still being assembled when it first
    // becomes targetable, so a single early resolve caches a nearly empty subtree and never recovers. Once a
    // second, and on any target change, is cheap and self healing.
    if (pTargetRoot)
    {
        m_sinceChainResolve += 1.0 / 90.0;

        if (m_chainFor != m_targetFormId)
        {
            m_chainFor = m_targetFormId;
            m_restCaptured = false;
            m_lastSubtreeCount[0] = 0;
            m_lastSubtreeCount[1] = 0;
            m_sinceChainResolve = 0.0;

            ResolveArmChains(pTargetRoot);
        }
        else if (m_sinceChainResolve >= 1.0)
        {
            m_sinceChainResolve = 0.0;
            ResolveArmChains(pTargetRoot);
        }
    }

    if (!pTargetRoot)
        return;

    /**
     * The pose has to be retargeted, not copied. A controller's world position is somewhere near the local
     * player's body, and the target actor is 1300 units away, so writing that position onto the target's hand
     * asks its arm to span the whole distance. The arm cannot, the solve clamps the elbow to what it can
     * reach, and the hand ends up flung across the world with the mesh stretched behind it. That is the long
     * arms, and no amount of chain solving fixes it.
     *
     * So the controller is expressed relative to the local player's own root and then re-applied relative to
     * the target's root. The target then reproduces the *pose* rather than the position. The real feature has
     * to send actor-relative coordinates for exactly this reason: a receiver's copy of a player sits at an
     * interpolated position that never quite matches the sender's.
     */
    const Xform cPlayerRoot = ReadXform(pPlayerRoot);
    const Xform cTargetRoot = ReadXform(pTargetRoot);

    const glm::mat3 cPlayerToTarget = cTargetRoot.Rotate * glm::transpose(cPlayerRoot.Rotate);

    for (size_t hand = 0; hand < 2; ++hand)
    {
        m_source[hand] = NodeRef{};
        m_target[hand] = NodeRef{};

        NiAVObject* pSource = ResolveSource(hand);
        if (!pSource)
            continue;

        const Xform cSourceXform = ReadXform(pSource);

        m_source[hand].pNode = pSource;
        m_source[hand].Name = m_sourceName[hand];
        m_source[hand].World = cSourceXform.Translate;
        m_source[hand].Resolved = true;

        // Same offset from the target's root as the controller has from the player's root.
        const glm::vec3 cGoal = cTargetRoot.Translate + cPlayerToTarget * (cSourceXform.Translate - cPlayerRoot.Translate);
        const glm::mat3 cGoalRotate = cPlayerToTarget * cSourceXform.Rotate;

        const ArmChain& cChain = m_chain[hand];

        if (!cChain.Hand.pNode)
            continue;

        NiAVObject* pHand = cChain.Hand.pNode;

        m_target[hand].pNode = pHand;
        m_target[hand].Name = kTargetBone[hand];
        m_target[hand].Resolved = true;

        // Where the hand actually gets written. The solve may pull this back to the edge of the arm's reach,
        // because a hand placed beyond what the forearm can span is the one thing guaranteed to stretch the
        // mesh no matter how well the rest of the chain is posed.
        glm::vec3 handGoal = cGoal;

        m_solve[hand] = SolveDebug{};

        if (m_solveArmChain && cChain.HasCore())
        {
            // The pose as the animation left it this frame. Bone lengths and the current bend plane both come
            // from here, before anything is written.
            const glm::vec3 cShoulder = ReadWorld(cChain.UpperArm.pNode);
            const glm::vec3 cElbow = ReadWorld(cChain.Forearm.pNode);
            const glm::vec3 cWrist = ReadWorld(pHand);

            const float cUpperLen = glm::length(cElbow - cShoulder);
            const float cLowerLen = glm::length(cWrist - cElbow);

            // Straight up from the shoulder, just inside full extension. Unmistakable if it lands, and it
            // isolates the write pipeline from the goal maths, which no earlier test did.
            if (m_staticPose)
                handGoal = cShoulder + glm::vec3(0.f, 0.f, (cUpperLen + cLowerLen) * 0.95f);

            const glm::vec3 cToTarget = handGoal - cShoulder;
            const float cRawDistance = glm::length(cToTarget);

            if (cRawDistance > 0.0001f && cUpperLen > 0.0001f && cLowerLen > 0.0001f)
            {
                // Clamp to what the arm can physically span, leaving it just short of straight so the bend
                // plane stays defined.
                const float cMin = std::abs(cUpperLen - cLowerLen) + 0.01f;
                const float cMax = cUpperLen + cLowerLen - 0.01f;
                const float cDistance = glm::clamp(cRawDistance, cMin, cMax);

                const glm::vec3 cAxis = cToTarget / cRawDistance;

                // Bring the hand back to whatever the arm can reach along the same direction. Without this the
                // forearm sits correctly at the shoulder while the hand stays out at the unreachable goal, and
                // the mesh spans the gap.
                handGoal = cShoulder + cAxis * cDistance;

                // Two bone analytic solve: where along the shoulder to target line the elbow projects, and
                // how far off that line it sits.
                const float cAlong = (cUpperLen * cUpperLen - cLowerLen * cLowerLen + cDistance * cDistance) / (2.f * cDistance);
                const float cOff = std::sqrt(std::max(0.f, cUpperLen * cUpperLen - cAlong * cAlong));

                // Keep the existing bend plane so the elbow does not flip between frames.
                glm::vec3 hint = cElbow - cShoulder;
                hint = hint - cAxis * glm::dot(hint, cAxis);

                if (glm::length(hint) < 0.0001f)
                    hint = glm::vec3(0.f, 0.f, 1.f) - cAxis * cAxis.z;

                if (glm::length(hint) > 0.0001f)
                {
                    hint = glm::normalize(hint);

                    const glm::vec3 cSolvedElbow = cShoulder + cAxis * cAlong + hint * cOff;

                    // Each bone is turned from where it points to where it should point, composed onto a
                    // reference orientation rather than the live one. Using the live rotation preserves the
                    // animation's roll as well as the rest orientation, which makes a walking actor's arms
                    // spin about their own length. The reference is stored relative to the actor's root, so
                    // it is brought back into world space with the root's current rotation and stays correct
                    // when the actor turns.
                    const glm::mat3 cRoot = cTargetRoot.Rotate;

                    const glm::mat3 cBaseUpper = m_restCaptured ? cRoot * m_restRotate[hand][0] : ReadPosed(cChain.UpperArm).Rotate;
                    const glm::mat3 cBaseFore = m_restCaptured ? cRoot * m_restRotate[hand][1] : ReadPosed(cChain.Forearm).Rotate;

                    const glm::vec3 cBaseUpperDir = m_restCaptured ? cRoot * m_restDir[hand][0] : cElbow - cShoulder;
                    const glm::vec3 cBaseForeDir = m_restCaptured ? cRoot * m_restDir[hand][1] : cWrist - cElbow;

                    const glm::mat3 cTurnUpper = RotationBetween(cBaseUpperDir, cSolvedElbow - cShoulder);

                    Xform newUpper{};
                    newUpper.Rotate = cTurnUpper * cBaseUpper;
                    newUpper.Translate = cShoulder;

                    // The forearm's reference direction is carried by the shoulder's turn first, the same way
                    // the hierarchy would carry it.
                    const glm::mat3 cTurnFore = RotationBetween(cTurnUpper * cBaseForeDir, handGoal - cSolvedElbow);

                    Xform newFore{};
                    newFore.Rotate = cTurnFore * (cTurnUpper * cBaseFore);
                    newFore.Translate = cSolvedElbow;

                    // Shoulder down. Each joint carries everything below it, so the forearm and hand are
                    // already rotated into place by the time their own deltas are worked out, exactly as
                    // the hierarchy would have done it.
                    if (m_isolate == 0 || m_isolate == 3)
                        PoseBoneAndSubtree(cChain.UpperArm, newUpper, cChain.UpperSubtree, m_flatLayoutConfirmed);

                    if (m_isolate == 0 || m_isolate == 2)
                        PoseBoneAndSubtree(cChain.Forearm, newFore, cChain.ForeSubtree, m_flatLayoutConfirmed);

                    m_solve[hand].Shoulder = cShoulder;
                    m_solve[hand].Elbow = cSolvedElbow;
                    m_solve[hand].Hand = handGoal;
                    m_solve[hand].UpperLen = cUpperLen;
                    m_solve[hand].LowerLen = cLowerLen;
                    m_solve[hand].GoalDistance = cRawDistance;
                    m_solve[hand].Clamped = cDistance != cRawDistance;
                    m_solve[hand].Solved = true;
                }
            }
        }

        // Whatever rotation is about to be written to the hand, measured. Column lengths away from 1 mean the
        // matrix carries a scale, and a negative determinant means it mirrors. Either one stretches or inverts
        // the mesh skinned to the bone, which is what the hand has been doing throughout.
        glm::mat3 handRotate = cGoalRotate;

        for (int col = 0; col < 3; ++col)
            m_solve[hand].HandColumnLength[col] = glm::length(handRotate[col]);

        m_solve[hand].HandDeterminant = glm::determinant(handRotate);

        // The wrist keeps whatever orientation the forearm just handed it, rather than being overwritten with
        // the controller's. A wand node and a hand bone do not share a rest orientation, so writing one onto
        // the other twists the palm away from the wrist and stretches the mesh between them, which is the
        // "palm should be together with the wrist" gap.
        //
        // Matching the palm to the player's real palm orientation needs the offset between the two rest frames
        // worked out, which is a separate job. Carrying the forearm's rotation is correct for the arm and
        // simply leaves the palm facing wherever the arm points.
        handRotate = ReadPosed(cChain.Hand).Rotate;

        // Last, so it wins over the deltas the two joints above just applied to it. The descendants come along
        // in HandSubtree.
        if (m_isolate == 0 || m_isolate == 1)
            PoseBoneAndSubtree(cChain.Hand, Xform{handRotate, handGoal}, cChain.HandSubtree, m_flatLayoutConfirmed);

        // Immediate read back, in the same breath as the write. This is the check that actually means
        // something: it confirms the write reached the memory the game reads a bone from. The cross frame
        // drift measured in CheckDrift cannot confirm it, because the animation legitimately re-poses the
        // skeleton every frame and so always reverts us.
        if (glm::length(ReadWorld(pHand) - handGoal) > 0.001f)
            ++m_writeFailures;

        m_written[hand] = handGoal;
        m_hasWritten[hand] = true;
        ++m_writes;
    }
}

/**
 * @brief Everything the first run needs to answer, logged once, without any UI interaction.
 *
 * Runs as soon as the player has 3D. Read only: it looks, names what it found and writes nothing.
 */
void SkeletonProbeService::RunDiscovery() noexcept
{
    spdlog::info("========== VR HAND SYNC PROBE ==========");

    ScanPlayerForWandNodes();

    // Candidate index 0 is the wand node, so that is what a real find looks like. Comparing the chosen name
    // against the fallback name instead would report a find whenever both were present.
    const bool cFoundWands = m_sourcePriority[0] == 0 && m_sourcePriority[1] == 0;

    if (cFoundWands)
        spdlog::info("PROBE RESULT 1: controller nodes found. Sources are \"{}\" and \"{}\".", m_sourceName[0], m_sourceName[1]);
    else
        spdlog::warn("PROBE RESULT 1: no controller node found in PlayerCharacter. Falling back to the player's own hand bones. Check the +0x offsets listed above for a name that looks like a wand or hand node.");

    // Which route reaches the source decides how the real feature reads it. The offset is the one that
    // matters for the controller nodes; the name search is what an SE build would use for hand bones.
    for (size_t hand = 0; hand < 2; ++hand)
    {
        const char* pSide = hand == 0 ? "left" : "right";

        NiAVObject* pFound = ResolveSource(hand);
        if (!pFound)
        {
            spdlog::warn("PROBE RESULT 2 ({}): \"{}\" did not resolve by offset or by name. Nothing to read.", pSide, m_sourceName[hand]);
            continue;
        }

        const glm::vec3 cWorld = ReadWorld(pFound);

        if (m_sourceOffset[hand])
            spdlog::info("PROBE RESULT 2 ({}): \"{}\" reads through PlayerCharacter+0x{:03X}, now at ({:.1f}, {:.1f}, {:.1f})", pSide, m_sourceName[hand], m_sourceOffset[hand], cWorld.x, cWorld.y, cWorld.z);
        else
            spdlog::info("PROBE RESULT 2 ({}): \"{}\" reads by name from the actor 3D, now at ({:.1f}, {:.1f}, {:.1f})", pSide, m_sourceName[hand], cWorld.x, cWorld.y, cWorld.z);
    }

    InstallHiggsHook();

    spdlog::info("PROBE: keys are F9 driving on/off, F10 arm chain solve, F11 static test pose, F12 cycles which single bone is posed. Not F4/F5/F6, which are taken by MagicService and by the connect/disconnect toggle.");

    PickNearestTarget();

    if (m_targetFormId)
        spdlog::info("PROBE: driving actor {:X} now. Watch for PROBE RESULT 3.", m_targetFormId);
    else
        spdlog::info("PROBE: nothing loaded to drive yet. Retrying once a second, so just walk up to a humanoid NPC and PROBE RESULT 3 will appear on its own.");

    spdlog::info("========================================");
}

// The answer to the question the probe exists for, restated once a second while driving so it is
// impossible to miss in the log.
void SkeletonProbeService::LogVerdict() noexcept
{
    const char* pWhere = m_higgsHookInstalled ? "post-VRIK callback" : "UpdateEvent";

    if (!m_writes)
    {
        spdlog::warn("PROBE RESULT 3 ({}): driving is on but nothing was written. Source or target bone did not resolve.", pWhere);
        return;
    }

    if (m_writeFailures)
    {
        spdlog::error("PROBE RESULT 3 ({}): {} of {} writes did not reach the bone at all. Wrong offset or wrong node.", pWhere, m_writeFailures, m_writes);
        return;
    }

    // Every write landing, and the animation reverting each one before the next frame, is the correct and
    // expected shape of a per frame bone override. It is what VRIK itself does. Whether it is late enough to
    // reach the screen cannot be read out of memory, so the criterion is the target actor's arms on screen.
    spdlog::info("PROBE RESULT 3 ({}): all {} writes landed. Animation re-posed the bone on {} of them ({:.0f} units), which is expected.", pWhere, m_writes, m_overwritten, m_worstDrift);

    // The pose as numbers. A posed arm is self consistent when the two segments kept their original lengths,
    // because those lengths are what the mesh was skinned against. If both errors are ~0 and the arm still
    // looks stretched on screen, the pose is right and something between here and the skin is the problem.
    for (size_t hand = 0; hand < 2; ++hand)
    {
        const SolveDebug& cSolve = m_solve[hand];
        const char* pSide = hand == 0 ? "left" : "right";

        if (!cSolve.Solved)
        {
            spdlog::warn("PROBE RESULT 4 ({}): no solve ran. Arm chain off, or a bone or length was degenerate.", pSide);
            continue;
        }

        const float cPosedUpper = glm::length(cSolve.Elbow - cSolve.Shoulder);
        const float cPosedLower = glm::length(cSolve.Hand - cSolve.Elbow);

        spdlog::info("PROBE RESULT 4 ({}): upper {:.2f} posed {:.2f} (err {:+.3f}), fore {:.2f} posed {:.2f} (err {:+.3f}), goal {:.1f} away{}", pSide, cSolve.UpperLen, cPosedUpper, cPosedUpper - cSolve.UpperLen, cSolve.LowerLen, cPosedLower, cPosedLower - cSolve.LowerLen, cSolve.GoalDistance, cSolve.Clamped ? " (clamped to reach)" : "");

        // A rotation has unit columns and determinant +1. Anything else is a scale, a shear or a mirror, and
        // writing it to a bone deforms the mesh skinned to that bone.
        const bool cUnit = std::abs(cSolve.HandColumnLength[0] - 1.f) < 0.01f && std::abs(cSolve.HandColumnLength[1] - 1.f) < 0.01f && std::abs(cSolve.HandColumnLength[2] - 1.f) < 0.01f;
        const bool cProper = std::abs(cSolve.HandDeterminant - 1.f) < 0.02f;

        if (cUnit && cProper)
            spdlog::info("PROBE RESULT 7 ({}): hand rotation is a clean rotation (columns {:.4f}/{:.4f}/{:.4f}, det {:.4f}).", pSide, cSolve.HandColumnLength[0], cSolve.HandColumnLength[1], cSolve.HandColumnLength[2], cSolve.HandDeterminant);
        else
            spdlog::error("PROBE RESULT 7 ({}): hand rotation is NOT a rotation. Columns {:.4f}/{:.4f}/{:.4f}, det {:.4f}. Writing this to a bone scales or mirrors the mesh skinned to it, which is the stretching.", pSide, cSolve.HandColumnLength[0], cSolve.HandColumnLength[1], cSolve.HandColumnLength[2], cSolve.HandDeterminant);
    }
}

namespace
{
// HIGGS callbacks take no user data, so the instance has to be reachable from a free function. There is
// exactly one probe and it lives as long as the World does.
SkeletonProbeService* s_pProbe = nullptr;
} // namespace

/**
 * @brief Moves the write to the one point in the frame that is expected to survive.
 *
 * A write from UpdateEvent is discarded because the game recomputes every bone's world transform from its
 * local one during the skeleton's downward pass, and the animation graph owns the locals. So the write has
 * to happen after that pass has already run for this frame.
 *
 * HIGGS's post VRIK callback is exactly that point: VRIK does its own arm IK by writing bones after the
 * skeleton is posed, and this fires straight after. If a write here survives, the feature is unblocked
 * without needing a new hook of our own. It costs nothing to try because the interface is already mirrored
 * in the client for the object sync work.
 *
 * HIGGS is optional, and without it the probe stays on UpdateEvent and keeps reporting that writes are lost.
 */
void SkeletonProbeService::InstallHiggsHook() noexcept
{
#if TP_SKYRIMVR
    if (m_higgsHookInstalled)
        return;

    IHiggsInterface001* pHiggs = HiggsAPI::Acquire();
    if (!pHiggs)
    {
        spdlog::warn("PROBE: HIGGS not present, so the write stays on UpdateEvent. Expect RESULT 3 to keep reporting overwrites.");
        return;
    }

    s_pProbe = this;

    // Cannot be unregistered, so this happens exactly once per process.
    pHiggs->AddPostVrikPostHiggsCallback(
        []()
        {
            if (s_pProbe)
                s_pProbe->Tick();
        });

    m_higgsHookInstalled = true;

    spdlog::info("PROBE: writing from HIGGS's post VRIK callback (build {}) instead of UpdateEvent.", pHiggs->GetBuildNumber());
#endif
}

void SkeletonProbeService::OnConnected(const ConnectedEvent&) noexcept
{
    m_connected = true;

    // Fresh run per session, so a reconnect re-measures rather than reporting stale offsets.
    m_discovered = false;
    m_sourcePriority[0] = std::numeric_limits<int>::max();
    m_sourcePriority[1] = std::numeric_limits<int>::max();
    m_sourceOffset[0] = 0;
    m_sourceOffset[1] = 0;
    m_targetFormId = 0;
    m_chainFor = 0;
    m_restCaptured = false;
    m_sinceTargetTry = 0.0;
    m_sinceLog = 0.0;
    m_writes = 0;
    m_overwritten = 0;
    m_writeFailures = 0;
    m_worstDrift = 0.f;
    m_hasWritten[0] = false;
    m_hasWritten[1] = false;
}

void SkeletonProbeService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_connected = false;

    // Stop touching bones on actors whose entities are about to be torn down.
    m_targetFormId = 0;
    m_hasWritten[0] = false;
    m_hasWritten[1] = false;
    m_target[0] = NodeRef{};
    m_target[1] = NodeRef{};
}

void SkeletonProbeService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    // Nothing in the main menu, nothing before a server connection.
    if (!m_connected)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (!m_discovered && pPlayer && pPlayer->GetNiNode())
    {
        m_discovered = true;
        RunDiscovery();
    }

    // Keys are read on the high bit, which is the key's actual up or down state, with the press edge detected
    // here.
    //
    // F9, F10 and F11 rather than F4, F5 and F6, because those were taken and it cost a wasted test. F6 is
    // DebugService's connect/disconnect toggle, so pressing it disarmed this probe through OnDisconnected and
    // looked exactly like the override switching off. F4 also drives MagicService. These three were checked
    // against every VK_ reference in the client before being picked.
    const bool cF4Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool cF5Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

    if (cF4Down && !m_wasF4Down)
    {
        m_enabled = !m_enabled;

        m_writes = 0;
        m_overwritten = 0;
        m_writeFailures = 0;
        m_worstDrift = 0.f;
        m_hasWritten[0] = false;
        m_hasWritten[1] = false;

        spdlog::info("PROBE: driving {} (target {:X}, arm chain {}, static pose {})", m_enabled ? "ON" : "OFF", m_targetFormId, m_solveArmChain ? "on" : "off", m_staticPose ? "on" : "off");
    }

    if (cF5Down && !m_wasF5Down)
    {
        m_solveArmChain = !m_solveArmChain;
        spdlog::info("PROBE: arm chain solve {}", m_solveArmChain ? "ON" : "OFF");
    }

    const bool cF6Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

    if (cF6Down && !m_wasF6Down)
    {
        m_staticPose = !m_staticPose;
        spdlog::info("PROBE RESULT 6: static pose {}. {}", m_staticPose ? "ON" : "OFF", m_staticPose ? "Both arms should now point straight up and hold still, ignoring your controllers. If they do, the write pipeline reaches the screen and the fault is in the goal. If they do not, the pose is not reaching the screen at all." : "Back to tracking the controllers.");
    }

    const bool cF12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;

    if (cF12Down && !m_wasF12Down)
    {
        m_isolate = (m_isolate + 1) % 4;

        static constexpr const char* kWhat[4] = {"whole chain", "HAND only", "FOREARM only", "UPPER ARM only"};

        spdlog::info("PROBE RESULT 8: posing {}. Watch which part of the body follows. Whatever does not follow any of these three is skinned to a bone this probe never writes.", kWhat[m_isolate]);
    }

    m_wasF4Down = cF4Down;
    m_wasF5Down = cF5Down;
    m_wasF6Down = cF6Down;
    m_wasF12Down = cF12Down;

    if (!m_enabled)
        return;

    // Nothing nearby is loaded at discovery time, and the target can wander off or die, so keep looking
    // until there is one. This is what removes the keypress from the critical path: once an actor with
    // arms is in range the probe starts on its own and the verdict appears without anyone pressing
    // anything.
    if (!m_targetFormId)
    {
        m_sinceTargetTry += acEvent.Delta;

        if (m_sinceTargetTry < 1.0)
            return;

        m_sinceTargetTry = 0.0;
        PickNearestTarget();

        if (!m_targetFormId)
            return;
    }

    // When the HIGGS hook is in place the write happens there instead, so that the read back measures a
    // full frame including the skeleton's downward pass. Doing it in both places would have each undo the
    // other's measurement.
    if (!m_higgsHookInstalled)
        Tick();

    m_sinceLog += acEvent.Delta;
    if (m_sinceLog >= 1.0)
    {
        m_sinceLog = 0.0;
        LogVerdict();
    }
}

// Counters are written here and read on the update thread for logging. They are plain rather than atomic
// because a torn count in a diagnostic line is not worth the ceremony, and the verdict is a ratio.
void SkeletonProbeService::Tick() noexcept
{
    if (!m_connected || !m_enabled || !m_targetFormId)
        return;

    CheckDrift();
    Apply();
}

void SkeletonProbeService::OnDraw() noexcept
{
    if (!m_world.GetDebugService().m_showDebugStuff)
        return;

    ImGui::Begin("VR hand sync probe");

    ImGui::TextUnformatted("Step 1: find out where things are");

    if (ImGui::Button("Dump player 3D tree to log"))
        DumpPlayerTree();

    if (ImGui::Button("Scan PlayerCharacter for node pointers"))
        ScanPlayerForWandNodes();

    ImGui::Text("Left source:  %s", m_sourceName[0].empty() ? "<none>" : m_sourceName[0].c_str());
    ImGui::Text("Right source: %s", m_sourceName[1].empty() ? "<none>" : m_sourceName[1].c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Step 2: pick something to drive");

    if (ImGui::Button("Pick nearest actor"))
        PickNearestTarget();

    ImGui::SameLine();
    ImGui::Text("target: %X", m_targetFormId);

    if (m_targetFormId && ImGui::Button("Dump target 3D tree to log"))
        DumpTargetTree();

    ImGui::Separator();
    ImGui::TextUnformatted("Step 3: does the write survive the frame?");

    ImGui::Checkbox("Drive target hands", &m_enabled);
    ImGui::Checkbox("Also solve forearm and upper arm", &m_solveArmChain);

    if (ImGui::Button("Reset counters"))
    {
        m_writes = 0;
        m_overwritten = 0;
        m_writeFailures = 0;
        m_worstDrift = 0.f;
    }

    for (size_t hand = 0; hand < 2; ++hand)
    {
        const char* pLabel = hand == 0 ? "left" : "right";

        ImGui::Separator();

        if (!m_source[hand].Resolved)
        {
            ImGui::Text("%s: source not resolved", pLabel);
            continue;
        }

        ImGui::Text("%s source (%.1f, %.1f, %.1f)", pLabel, m_source[hand].World.x, m_source[hand].World.y, m_source[hand].World.z);

        if (!m_target[hand].Resolved)
        {
            ImGui::Text("%s: target bone not resolved", pLabel);
            continue;
        }

        ImGui::Text("%s wrote  (%.1f, %.1f, %.1f)", pLabel, m_written[hand].x, m_written[hand].y, m_written[hand].z);
        ImGui::Text("%s found  (%.1f, %.1f, %.1f)", pLabel, m_readBack[hand].x, m_readBack[hand].y, m_readBack[hand].z);
        ImGui::Text("%s drift  %.3f", pLabel, m_drift[hand]);
    }

    ImGui::Separator();
    ImGui::Text("writes %u, overwritten %u, worst drift %.3f", m_writes, m_overwritten, m_worstDrift);

    if (m_writes && !m_overwritten)
        ImGui::TextUnformatted("Writes are surviving. UpdateEvent is a good enough hook.");
    else if (m_overwritten)
        ImGui::TextUnformatted("Writes are being overwritten. A later frame hook is needed.");

    ImGui::End();
}
