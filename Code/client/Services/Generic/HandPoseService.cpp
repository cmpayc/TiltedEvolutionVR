#include <TiltedOnlinePCH.h>

#include <Services/HandPoseService.h>
#include <Services/ImguiService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/RequestHandPose.h>
#include <Messages/NotifyHandPose.h>

#include <Games/Memory.h>
#include <Misc/BSFixedString.h>
#include <NetImmerse/NiNode.h>
#include <PlayerCharacter.h>

#include <Components.h>
#include <World.h>
#include <Utils.h>

namespace
{
/**
 * @brief NiAVObject transform layout, from the measurement documented in ObjectService.cpp.
 *
 * Three NiTransforms back to back: local, world, previousWorld, each 0x34 bytes. Same on SE and VR.
 */
constexpr size_t kLocalRotate = 0x48;
constexpr size_t kWorldRotate = 0x7C;
constexpr size_t kWorldTranslate = 0xA0;

/**
 * @brief BSFlattenedBoneTree's bone array, measured on SkyrimVR 1.4.15. See PROGRESS.md session 9.
 *
 * The skinning reads bone transforms from here, not from the bone nodes, so posing only the nodes produces a
 * geometrically perfect skeleton that the renderer ignores.
 */
constexpr size_t kTreeBoneArrayPtr = 0x158;
constexpr size_t kBoneEntryStride = 0x80;
constexpr size_t kBoneEntryLocal = 0x00;
constexpr size_t kBoneEntryWorld = 0x34;
constexpr size_t kBoneEntryIndices = 0x68;
constexpr size_t kBoneEntryRefNode = 0x70;
constexpr size_t kBoneIndexFields = 4;
constexpr size_t kMaxBones = 1024;

/**
 * @brief Where SkyrimVR keeps the controller nodes, measured by scanning PlayerCharacter for named nodes.
 *
 * These are not part of the actor's 3D and cannot be found by a name search from it. The names are checked at
 * runtime before the offsets are used, so a build where they moved reports that rather than reading rubbish.
 */
constexpr size_t kWandNodeOffset[2] = {0x490, 0x4F8};
constexpr const char* kWandNodeName[2] = {"LeftWandNode", "RightWandNode"};

// The headset node, from the same measured block as the wands. Used to tell what the viewer can see.
constexpr size_t kHmdNodeOffset = 0x570;
constexpr const char* kHmdNodeName = "HmdNode";

// Only reject what is clearly behind the viewer. Zero would be a flat ninety degrees either side; this leaves
// a margin so somebody at the edge of vision, or a head turn the pose has not caught up with, still counts as
// visible. Freezing the arms of a player you can see is worse than posing one you cannot.
constexpr float kInViewDot = -0.25f;

/**
 * @brief Shaping applied to a received palm before the arm is solved to it, in the character's own frame.
 *
 * A headset reports where the hands physically are, and a Skyrim skeleton is not the player. Two goals in
 * particular produce arms that bend in ways no arm does: one tucked against the chest, and one that has crossed
 * behind the torso. Both come out of the two bone solve looking broken rather than merely wrong.
 *
 * Skyrim's unit is roughly 1.4cm, so about 70 to the metre.
 */
constexpr float kUnitsPerMetre = 70.f;

// How far out from the body's centre line each hand is pushed. Ten centimetres.
constexpr float kOutwardOffset = 0.10f * kUnitsPerMetre;

// How far forward of the body's centre a hand is allowed to get. Zero is the plane through the middle of the
// torso, which still leaves a hand inside the chest: a torso is a good fifteen centimetres deep from its centre
// to its front. Clamping in front of that keeps hands off the body rather than merely out of the back.
constexpr float kMinForwardOffset = 0.18f * kUnitsPerMetre;

// How far the elbow may lean backwards, as the backward component of a unit direction, so -1 is straight back
// and 0 forbids any lean at all. The small allowance that was here let elbows sit inside the torso, so they are
// now held at or in front of the shoulder line. Slightly stiffer than a real arm, and it keeps them out of the
// body, which reads better than anatomically correct elbows buried in the chest.
constexpr float kElbowBackwardLimit = 0.f;

// How far above the shoulder a head bone has to read before the measurement is believed. A standing skeleton
// clears this comfortably; one caught mid animation put its head below the shoulder entirely.
constexpr float kMinHeadAboveShoulder = 8.f;

/**
 * @brief Shaping for a crouched character.
 *
 * Palms are anchored to the character's root, which stays at the feet, so crouching drops the body while the
 * hands stay where a standing player's hands are. The player has not physically crouched, so nothing in the
 * tracking accounts for it and the hands end up floating around the character's head.
 *
 * Dropping them by roughly the amount the body sinks puts them back on the torso. Skyrim's crouch also leads
 * with the left shoulder, so the left hand is nudged forward to sit with it rather than inside the chest.
 */
constexpr float kCrouchDrop = 0.45f * kUnitsPerMetre;
constexpr float kCrouchLeftForward = 0.30f * kUnitsPerMetre;

// Both hands also sit left of where they belong while crouched, by about a third of a metre. The sneak pose
// turns the torso, which is the same thing that puts the left shoulder in front, so hands measured against the
// root's un-turned frame end up off to one side of the body they are supposed to belong to.
constexpr float kCrouchRightShift = 0.15f * kUnitsPerMetre;

constexpr const char* kHandBone[2] = {"NPC L Hand [LHnd]", "NPC R Hand [RHnd]"};
constexpr const char* kForearmBone[2] = {"NPC L Forearm [LLar]", "NPC R Forearm [RLar]"};
constexpr const char* kUpperArmBone[2] = {"NPC L UpperArm [LUar]", "NPC R UpperArm [RUar]"};

// Palm positions go out at this rate while they are moving.
constexpr double kSendInterval = 1.0 / 30.0;

// Below this, a palm counts as not having moved and nothing is sent. A still player then costs nothing.
constexpr float kSendThreshold = 0.5f;

// A message goes out at least this often even when nothing has moved, so a receiver always hears about hands
// being switched off and can tell a still player from one that stopped talking.
constexpr double kKeepAliveInterval = 0.25;

// How long a receiver keeps posing after the last message before handing the arms back to the animation.
constexpr double kPoseTimeout = 1.0;

// How long to wait before trying again after a failed resolve. Resolving walks the whole bone array with a
// VirtualQuery per entry, so retrying it every frame is enough to be felt as stutter.
constexpr double kResolveRetryInterval = 1.0;

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

const char* GetNodeName(const NiAVObject* acpNode) noexcept
{
    constexpr size_t cNameOffset = 0x10;

    if (!IsReadable(acpNode, cNameOffset + sizeof(const char*)))
        return nullptr;

    return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(acpNode) + cNameOffset);
}

bool NodeNameIs(const NiAVObject* acpNode, const char* acpExpected) noexcept
{
    const char* pName = GetNodeName(acpNode);
    if (!pName || !IsReadable(pName, 1))
        return false;

    // Bounded compare: the name lives in game memory and the length is not known up front.
    for (size_t i = 0; i < 128; ++i)
    {
        if (!IsReadable(pName + i, 1))
            return false;

        if (pName[i] != acpExpected[i])
            return false;

        if (acpExpected[i] == '\0')
            return true;
    }

    return false;
}

struct Xform
{
    glm::mat3 Rotate{1.f};
    glm::vec3 Translate{};
};

Xform ReadXformAt(const uint8_t* acpBase) noexcept
{
    const auto* pR = reinterpret_cast<const float*>(acpBase);
    const auto* pT = reinterpret_cast<const float*>(acpBase + 0x24);

    Xform out{};

    // Stored row major with the bone's local axes as columns; glm is column major, so the indices swap.
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

Xform ReadNodeWorld(const NiAVObject* acpNode) noexcept
{
    return ReadXformAt(reinterpret_cast<const uint8_t*>(acpNode) + kWorldRotate);
}

Xform Compose(const Xform& acFirst, const Xform& acSecond) noexcept
{
    Xform out{};

    out.Rotate = acFirst.Rotate * acSecond.Rotate;
    out.Translate = acFirst.Rotate * acSecond.Translate + acFirst.Translate;

    return out;
}

/**
 * @brief The shortest rotation taking one direction onto another, by Rodrigues' formula.
 *
 * Bones are never given a rotation built from scratch. Skyrim gives each one a rest orientation and a roll
 * about its own length, and aiming a fresh frame down the bone throws both away, which shows up as a shoulder
 * facing backwards and a palm torn off the wrist.
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

    if (glm::length(axis) < 0.0001f)
    {
        const glm::vec3 cReference = std::abs(cFrom.x) < 0.9f ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 1.f, 0.f);
        axis = glm::cross(cFrom, cReference);
    }

    axis = glm::normalize(axis);

    const float cAngle = std::acos(cDot);
    const glm::mat3 cSkew(0.f, axis.z, -axis.y, -axis.z, 0.f, axis.x, axis.y, -axis.x, 0.f);

    return glm::mat3(1.f) + cSkew * std::sin(cAngle) + (cSkew * cSkew) * (1.f - std::cos(cAngle));
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

const char* GetRttiName(NiAVObject* apNode) noexcept
{
    // A virtual call needs a vtable that is actually callable, not merely memory that reads.
    if (!IsReadable(apNode, sizeof(void*)))
        return nullptr;

    const auto* pVtable = *reinterpret_cast<void* const* const*>(apNode);
    if (!IsReadable(pVtable, sizeof(void*) * 2) || !pVtable[1])
        return nullptr;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pVtable[1], &info, sizeof(info)) || info.State != MEM_COMMIT)
        return nullptr;

    constexpr DWORD cExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cExecutable))
        return nullptr;

    const auto* pRtti = reinterpret_cast<const char* const*>(apNode->GetRTTI());

    return IsReadable(pRtti, sizeof(const char*)) ? *pRtti : nullptr;
}

// Depth first search by RTTI type name. BSFlattenedBoneTree cannot be found by node name.
NiAVObject* FindByRttiName(NiAVObject* apNode, const char* acpWanted, const uint32_t aDepth) noexcept
{
    if (!IsReadable(apNode, kNiAVObjectSize) || aDepth > 20)
        return nullptr;

    const char* pRtti = GetRttiName(apNode);

    if (pRtti && IsReadable(pRtti, 1))
    {
        bool match = true;
        for (size_t i = 0; i < 64 && match; ++i)
        {
            if (!IsReadable(pRtti + i, 1) || pRtti[i] != acpWanted[i])
                match = false;
            else if (acpWanted[i] == '\0')
                return apNode;
        }
    }

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

uint8_t* GetBoneArray(NiAVObject* apTree) noexcept
{
    if (!IsReadable(apTree, kTreeBoneArrayPtr + sizeof(void*)))
        return nullptr;

    auto* pBlock = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(apTree) + kTreeBoneArrayPtr);

    return IsReadable(pBlock, kBoneEntryStride) ? pBlock : nullptr;
}

/**
 * @brief How many entries the bone array holds, read from the tree and then proved against the array.
 *
 * Counting readable entries instead is not the same thing and is dangerous: readable memory runs past the end
 * of the heap block, so unrelated heap gets treated as bone entries and written to. That crashed the game
 * during development. Every entry below the count must be readable and carry a refNode that is null or a
 * readable object, or the count is rejected.
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

// Which of the four int16s at entry+0x68 is the parent index, decided by the one that maps forearm to upper
// arm and hand to forearm. Nothing is assumed about the slot.
int FindParentIndexField(uint8_t* apBlock, const size_t acCount, const uint8_t* acpUpper, const uint8_t* acpFore, const uint8_t* acpHand) noexcept
{
    if (!apBlock || !acpUpper || !acpFore || !acpHand)
        return -1;

    const int64_t cUpper = (acpUpper - apBlock) / static_cast<int64_t>(kBoneEntryStride);
    const int64_t cFore = (acpFore - apBlock) / static_cast<int64_t>(kBoneEntryStride);

    for (size_t field = 0; field < kBoneIndexFields; ++field)
    {
        if (ReadBoneIndex(acpFore, field) == cUpper && ReadBoneIndex(acpHand, field) == cFore && cUpper >= 0 && static_cast<size_t>(cUpper) < acCount)
            return static_cast<int>(field);
    }

    return -1;
}

uint8_t* FindBoneEntry(uint8_t* apBlock, const size_t acCount, const NiAVObject* acpNode) noexcept
{
    if (!apBlock || !acpNode)
        return nullptr;

    for (size_t i = 0; i < acCount; ++i)
    {
        uint8_t* pEntry = apBlock + i * kBoneEntryStride;

        if (*reinterpret_cast<const NiAVObject* const*>(pEntry + kBoneEntryRefNode) == acpNode)
            return pEntry;
    }

    return nullptr;
}

/**
 * @brief Every descendant of a node in the child tree.
 *
 * The bone array is not enough on its own. Bones that were flattened away, such as fingers, exist only in the
 * array, but attachment nodes exist only in the child tree: WEAPON and SHIELD, which a drawn weapon hangs off,
 * plus the magic and anim object nodes. Collecting from one source alone leaves the other kind behind, which
 * is a drawn weapon staying at the hip while the hand moves.
 */
void CollectNodeSubtree(NiAVObject* apNode, const uint32_t aDepth, std::vector<NiAVObject*>& aOut) noexcept
{
    if (!IsReadable(apNode, sizeof(NiNode)) || aDepth > 12)
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
        CollectNodeSubtree(pChild, aDepth + 1, aOut);
    }
}

// Adds a bone unless it is already in the list. A bone with both a node and an array slot is reachable from
// both sources, and carrying it twice would apply the joint's delta to it twice.
void AddUnique(std::vector<HandPoseService::PosedNode>& aList, const HandPoseService::PosedNode& acNode) noexcept
{
    for (const HandPoseService::PosedNode& cExisting : aList)
    {
        if (acNode.pNode && cExisting.pNode == acNode.pNode)
            return;

        if (acNode.pFlatEntry && cExisting.pFlatEntry == acNode.pFlatEntry)
            return;
    }

    aList.push_back(acNode);
}

// Copies a node's name into a buffer, bounded by what is readable.
bool CopyNodeName(const NiAVObject* acpNode, char* apOut, const size_t aMax) noexcept
{
    const char* pName = GetNodeName(acpNode);

    if (!pName)
        return false;

    for (size_t i = 0; i + 1 < aMax; ++i)
    {
        if (!IsReadable(pName + i, 1))
            return false;

        apOut[i] = pName[i];

        if (pName[i] == '\0')
            return true;

        if (pName[i] < 0x20 || pName[i] > 0x7E)
            return false;
    }

    return false;
}

Xform ReadPosed(const HandPoseService::PosedNode& acTarget) noexcept
{
    if (acTarget.pNode)
        return ReadNodeWorld(acTarget.pNode);

    if (acTarget.pFlatEntry)
        return ReadXformAt(acTarget.pFlatEntry + kBoneEntryWorld);

    return Xform{};
}

// Records or checks one cached bone pointer's identity. See PosedNode::pVTable for why.
bool AuditOnePosedNode(HandPoseService::PosedNode& aNode, const bool aCapture) noexcept
{
    bool intact = true;

    if (aNode.pNode)
    {
        if (!IsReadable(aNode.pNode, sizeof(void*)))
        {
            // The page behind it is gone, which is as stale as a pointer gets.
            if (aCapture)
                aNode.pVTable = nullptr;

            intact = false;
        }
        else
        {
            const void* const cpVTable = *reinterpret_cast<const void* const*>(aNode.pNode);

            if (aCapture)
                aNode.pVTable = cpVTable;
            else if (aNode.pVTable && aNode.pVTable != cpVTable)
                intact = false;
        }
    }

    // The array slot is checked separately, because WriteBone writes through it even when pNode is null.
    if (aNode.pFlatEntry)
    {
        if (!IsReadable(aNode.pFlatEntry, kBoneEntryStride))
        {
            if (aCapture)
                aNode.pFlatRefNode = nullptr;

            intact = false;
        }
        else
        {
            const void* const cpRefNode = *reinterpret_cast<const void* const*>(aNode.pFlatEntry + kBoneEntryRefNode);

            if (aCapture)
                aNode.pFlatRefNode = cpRefNode;
            else if (aNode.pFlatRefNode && aNode.pFlatRefNode != cpRefNode)
                intact = false;
        }
    }

    // A pointer whose witness could not be read at resolve time is not evidence either way, so it is left
    // alone rather than reported as stale on every frame for ever.
    return intact;
}

// Writes a world transform, and the matching local, to the node and to the array slot. World alone is
// discarded by anything that recomputes world from local.
void WriteBone(const HandPoseService::PosedNode& acTarget, const Xform& acWanted, const bool aUpdateLocal) noexcept
{
    if (!acTarget.pNode)
    {
        if (acTarget.pFlatEntry)
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

        // local_new = local_old * (inverse(world_old) * world_new), which needs no parent transform.
        Xform delta{};
        delta.Rotate = glm::transpose(cWorldOld.Rotate) * acWanted.Rotate;
        delta.Translate = glm::transpose(cWorldOld.Rotate) * (acWanted.Translate - cWorldOld.Translate);

        localNew = Compose(cLocalOld, delta);
        haveLocal = true;

        WriteXformAt(pNodeBytes + kLocalRotate, localNew);
    }

    WriteXformAt(pNodeBytes + kWorldRotate, acWanted);

    if (!acTarget.pFlatEntry)
        return;

    WriteXformAt(acTarget.pFlatEntry + kBoneEntryWorld, acWanted);

    if (haveLocal)
        WriteXformAt(acTarget.pFlatEntry + kBoneEntryLocal, localNew);
}

// Poses one joint and carries everything below it by the same rigid motion. Children are not derived from
// locals at this point in the frame, so every descendant has to be moved by hand.
void PoseJoint(const HandPoseService::PosedNode& acBone, const Xform& acWanted, const std::vector<HandPoseService::PosedNode>& acSubtree) noexcept
{
    if (!acBone.pNode && !acBone.pFlatEntry)
        return;

    const Xform cCurrent = ReadPosed(acBone);

    const glm::mat3 cDeltaRotate = acWanted.Rotate * glm::transpose(cCurrent.Rotate);
    const glm::vec3 cDeltaTranslate = acWanted.Translate - cDeltaRotate * cCurrent.Translate;

    WriteBone(acBone, acWanted, true);

    for (const HandPoseService::PosedNode& cNode : acSubtree)
    {
        Xform node = ReadPosed(cNode);

        node.Rotate = cDeltaRotate * node.Rotate;
        node.Translate = cDeltaRotate * node.Translate + cDeltaTranslate;

        // A rigid parent motion leaves a child's local transform unchanged by definition.
        WriteBone(cNode, node, false);
    }
}

} // namespace

HandPoseService::HandPoseService(entt::dispatcher& aDispatcher, World& aWorld, TransportService& aTransport, ImguiService& aImguiService)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&HandPoseService::OnUpdate>(this);

    // The write point. This fires from RenderSystemD3D11::OnRender, which the game's frame end hook calls on
    // the render thread: one thread, every frame, at a fixed point. See OnRenderPose for why that matters.
    m_drawConnection = aImguiService.OnDraw.connect<&HandPoseService::OnRenderPose>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&HandPoseService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&HandPoseService::OnDisconnected>(this);
    m_handPoseConnection = aDispatcher.sink<NotifyHandPose>().connect<&HandPoseService::OnHandPoseNotify>(this);
}

void HandPoseService::OnConnected(const ConnectedEvent&) noexcept
{
    m_connected = true;
    m_hasSent = false;

    // The server id is per connection. The wand check is per process, so it is deliberately not reset here.
    m_localServerId = 0;
}

void HandPoseService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_connected = false;

    std::scoped_lock lock(m_remotesMutex);
    m_remotes.clear();
}

void HandPoseService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!m_connected)
        return;

    // F10, checked against every VK_ reference in the client: free. Read on the high bit with the edge tracked
    // here, because the low "pressed since last call" bit is shared system wide and gets eaten by anything else
    // polling the same key.
    const bool cF10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

    if (cF10Down && !m_wasF10Down)
    {
        m_posingEnabled = !m_posingEnabled;

        spdlog::info("Hand sync: posing other players' arms is now {}. Sending is unchanged.", m_posingEnabled ? "ON" : "OFF");

        // Leave nothing half posed. The animation re-poses the whole skeleton every frame, so simply not
        // writing hands the arms straight back.
        if (!m_posingEnabled)
        {
            std::scoped_lock lock(m_remotesMutex);

            for (auto& [id, hands] : m_remotes)
                hands.HasPose = false;
        }
    }

    m_wasF10Down = cF10Down;

    m_sinceSend += acEvent.Delta;

    if (m_sinceSend < kSendInterval)
        return;

    m_sinceSend = 0.0;

    SendLocalPose();
}

/**
 * @brief Reads the local player's controllers and sends them for their own character.
 *
 * Positions go out relative to the character's own root. A receiver's copy of this player sits at an
 * interpolated position that never quite matches, so a world position would hang the hands off the body by
 * however far the two disagree.
 */
void HandPoseService::SendLocalPose() noexcept
{
#if TP_SKYRIMVR
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    NiNode* pRoot = pPlayer->GetNiNode();
    if (!pRoot)
        return;

    /**
     * Hands are only synced with a weapon put away.
     *
     * A drawn weapon is held and swung by animations that the game drives from combat state, and overriding
     * the arms then fights those animations rather than adding anything: the weapon follows whichever node the
     * game attached it to, so a posed arm can carry a sheathed weapon into the hand or leave a drawn one on the
     * hip. Standing unarmed is where hand tracking is worth having and where nothing else is competing for the
     * arms.
     *
     * The state is judged here, on the client that owns the character, and sent. The receiving client's copy of
     * this actor does not reliably agree about whether a weapon is out, so deciding there would release or
     * freeze the wrong arms.
     */
    const bool cActive = !pPlayer->actorState.IsWeaponDrawn();

    if (!m_hasActiveState || cActive != m_wasActive)
        spdlog::info("Hand sync: local hands are now {} (weapon {})", cActive ? "synced" : "not synced", cActive ? "away" : "drawn");

    const Xform cRoot = ReadNodeWorld(pRoot);
    const glm::mat3 cRootInverse = glm::transpose(cRoot.Rotate);

    glm::vec3 palm[2]{};

    for (size_t hand = 0; cActive && hand < 2; ++hand)
    {
        const auto* pSlot = reinterpret_cast<const uint8_t*>(pPlayer) + kWandNodeOffset[hand];

        auto* pWand = *reinterpret_cast<NiAVObject* const*>(pSlot);

        if (!pWand)
            return;

        // The offsets were measured rather than documented, so the nodes they point at are checked against
        // their names. Once per session: the check walks the name a character at a time with a VirtualQuery
        // per step, which at thirty sends a second is enough to be felt.
        if (!m_wandOffsetsChecked)
        {
            if (!IsReadable(pWand, kNiAVObjectSize) || !NodeNameIs(pWand, kWandNodeName[hand]))
            {
                spdlog::error("PlayerCharacter+0x{:03X} is not {} on this build, so hand sync is off. The offset needs re-measuring.", kWandNodeOffset[hand], kWandNodeName[hand]);

                m_wandOffsetsChecked = true;
                m_wandOffsetsValid = false;

                return;
            }

            if (hand == 1)
            {
                m_wandOffsetsChecked = true;
                m_wandOffsetsValid = true;

                spdlog::info("Hand sync: controller nodes confirmed at PlayerCharacter+0x{:03X} and +0x{:03X}", kWandNodeOffset[0], kWandNodeOffset[1]);
            }
        }
        else if (!m_wandOffsetsValid)
        {
            return;
        }

        palm[hand] = cRootInverse * (ReadNodeWorld(pWand).Translate - cRoot.Translate);
    }

    // A player holding still costs nothing, but the state still has to be repeated occasionally so a receiver
    // that missed the packet turning hands off does not leave an actor's arms frozen indefinitely.
    m_sinceKeepAlive += kSendInterval;

    const bool cDue = m_sinceKeepAlive >= kKeepAliveInterval;
    const bool cChanged = !m_hasActiveState || cActive != m_wasActive;

    if (!cDue && !cChanged && m_hasSent && glm::length(palm[0] - m_lastSent[0]) < kSendThreshold && glm::length(palm[1] - m_lastSent[1]) < kSendThreshold)
        return;

    m_sinceKeepAlive = 0.0;

    // Resolved once per connection. Finding it means scanning every entity carrying a form id, and a loaded
    // cell has enough of those that repeating it thirty times a second shows up as stutter.
    if (!m_localServerId)
    {
        auto view = m_world.view<FormIdComponent>();
        const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });

        if (it == view.end())
            return;

        const std::optional<uint32_t> cServerId = Utils::GetServerId(*it);
        if (!cServerId.has_value())
            return;

        m_localServerId = cServerId.value();
    }

    // How high the headset actually is above this player's feet, which is what makes the palms meaningful on a
    // character of a different size. Zero if the node is unreadable, which the receiver treats as "no scaling".
    float eyeHeight = 0.f;

    if (const auto* pHmdSlot = reinterpret_cast<const uint8_t*>(pPlayer) + kHmdNodeOffset)
    {
        auto* pHmd = *reinterpret_cast<NiAVObject* const*>(pHmdSlot);

        if (pHmd && IsReadable(pHmd, kNiAVObjectSize))
            eyeHeight = (cRootInverse * (ReadNodeWorld(pHmd).Translate - cRoot.Translate)).z;
    }

    RequestHandPose request{};
    request.Id = m_localServerId;
    request.EyeHeight = eyeHeight;
    request.LeftPalm = palm[0];
    request.RightPalm = palm[1];
    request.HandsActive = cActive;

    m_transport.Send(request);

    m_lastSent[0] = palm[0];
    m_lastSent[1] = palm[1];
    m_wasActive = cActive;
    m_hasActiveState = true;
    m_hasSent = true;
#endif
}

bool HandPoseService::IsInView(const glm::vec3& acWorldPosition) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return true;

    const auto* pSlot = reinterpret_cast<const uint8_t*>(pPlayer) + kHmdNodeOffset;

    auto* pHmd = *reinterpret_cast<NiAVObject* const*>(pSlot);

    // No headset node means no opinion, so pose rather than silently stop.
    if (!pHmd || !IsReadable(pHmd, kNiAVObjectSize))
        return true;

    const Xform cHmd = ReadNodeWorld(pHmd);

    const glm::vec3 cToTarget = acWorldPosition - cHmd.Translate;
    const float cDistance = glm::length(cToTarget);

    // Standing on top of each other: no meaningful direction, so do not start culling.
    if (cDistance < 1.f)
        return true;

    // Skyrim's convention is X right, Y forward, Z up, so the second column of the rotation is the node's
    // forward axis. Verified by the log line in PoseActor flipping when the viewer turns around.
    const glm::vec3 cForward = glm::vec3(cHmd.Rotate[1]);

    return glm::dot(cForward, cToTarget / cDistance) > kInViewDot;
}

void HandPoseService::OnHandPoseNotify(const NotifyHandPose& acMessage) noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto it = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.Id](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (it == std::end(remoteView))
        return;

    const uint32_t cFormId = remoteView.get<FormIdComponent>(*it).Id;

    std::scoped_lock lock(m_remotesMutex);

    RemoteHands& hands = m_remotes[acMessage.Id];

    // A form id change means a different actor, so anything resolved against the old one is stale.
    if (hands.FormId != cFormId)
    {
        hands = RemoteHands{};
        hands.FormId = cFormId;
    }

    // An inactive sender leaves the pose behind rather than overwriting it, so nothing snaps if hands come
    // back. Not posing is all that is needed to hand the arms back: the animation re-poses the whole skeleton
    // every frame, so the moment we stop writing it takes over.
    hands.Age = 0.0;
    hands.HasPose = acMessage.HandsActive;

    if (!acMessage.HandsActive)
        return;

    hands.Palm[0] = acMessage.LeftPalm;
    hands.Palm[1] = acMessage.RightPalm;
    hands.SenderEyeHeight = acMessage.EyeHeight;
}

/**
 * @brief Poses every remote player's arms, from the game's render hook.
 *
 * The write point matters as much as the pose. A bone written from the client's UpdateEvent never reaches the
 * screen, because the game recomputes world transforms from locals during the skeleton's downward pass and the
 * animation graph owns the locals. HIGGS's post VRIK callback is late enough, but it comes from a job pool: the
 * renderer reads a bone while it is being written and draws the half-updated matrix as a black band across the
 * screen. Restricting those writes to a single thread reduced the bands without removing them.
 *
 * BSGraphics::Hook_StopTimer calls RenderSystemD3D11::OnRender on the render thread, every frame, at a fixed
 * point. One thread, one call, no race, and it needs nothing from HIGGS.
 */
void HandPoseService::OnRenderPose() noexcept
{
    if (!m_connected || !m_posingEnabled)
        return;

    // Aged here rather than on the update thread. Both would need the same lock, and the update thread taking
    // it every frame left the render thread waiting on it.
    const auto cNow = std::chrono::high_resolution_clock::now();
    const double cDelta = m_lastPoseTime.time_since_epoch().count() ? std::chrono::duration<double>(cNow - m_lastPoseTime).count() : 0.0;

    m_lastPoseTime = cNow;

    std::scoped_lock lock(m_remotesMutex);

    for (auto& [id, hands] : m_remotes)
    {
        hands.Age += cDelta;
        hands.SinceFailedResolve += cDelta;

        if (hands.Age > kPoseTimeout)
            hands.HasPose = false;

        if (hands.HasPose)
            PoseActor(hands);
    }
}

/**
 * @brief Finds both arms, their descendants and their slots in the flattened array, once per actor.
 *
 * Descendants come from the array's parent indices rather than from NiNode children. Most bones below the
 * wrist have an array entry and no node at all, so a child walk finds none of them and a posed palm moves while
 * every finger stays behind and stretches.
 */
bool HandPoseService::ResolveChains(RemoteHands& aHands, Actor* apActor) noexcept
{
    NiNode* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    aHands.Chain[0] = ArmChain{};
    aHands.Chain[1] = ArmChain{};
    aHands.LayoutConfirmed = false;

    // Cleared up front so a resolve that fails half way cannot leave the previous run's flag standing, which
    // would stop the retry from ever happening and leave the arms with empty chains for good.
    aHands.RestCaptured = false;

    NiAVObject* pTree = FindByRttiName(pRoot, "BSFlattenedBoneTree", 0);
    uint8_t* pBoneArray = pTree ? GetBoneArray(pTree) : nullptr;
    const size_t cBoneCount = FindBoneCount(pTree, pBoneArray);

    if (!cBoneCount)
        return false;

    for (size_t hand = 0; hand < 2; ++hand)
    {
        ArmChain& chain = aHands.Chain[hand];

        chain.UpperArm.pNode = FindByName(pRoot, kUpperArmBone[hand]);
        chain.Forearm.pNode = FindByName(pRoot, kForearmBone[hand]);
        chain.Hand.pNode = FindByName(pRoot, kHandBone[hand]);

        if (!chain.HasCore())
            return false;

        for (PosedNode* pTarget : {&chain.UpperArm, &chain.Forearm, &chain.Hand})
            pTarget->pFlatEntry = FindBoneEntry(pBoneArray, cBoneCount, pTarget->pNode);

        const int cParentField = FindParentIndexField(pBoneArray, cBoneCount, chain.UpperArm.pFlatEntry, chain.Forearm.pFlatEntry, chain.Hand.pFlatEntry);

        if (cParentField < 0)
            return false;

        for (size_t i = 0; i < cBoneCount; ++i)
        {
            uint8_t* pEntry = pBoneArray + i * kBoneEntryStride;

            if (pEntry == chain.UpperArm.pFlatEntry || pEntry == chain.Forearm.pFlatEntry || pEntry == chain.Hand.pFlatEntry)
                continue;

            std::vector<PosedNode>* pOwner = nullptr;
            int16_t walk = static_cast<int16_t>(i);

            for (size_t step = 0; step < 64; ++step)
            {
                walk = ReadBoneIndex(pBoneArray + static_cast<size_t>(walk) * kBoneEntryStride, static_cast<size_t>(cParentField));

                if (walk < 0 || static_cast<size_t>(walk) >= cBoneCount)
                    break;

                uint8_t* pAncestor = pBoneArray + static_cast<size_t>(walk) * kBoneEntryStride;

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

            AddUnique(*pOwner, PosedNode{IsReadable(pRefNode, kNiAVObjectSize) ? pRefNode : nullptr, pEntry});
        }

        // Then the child tree, for the nodes the array does not know about. A drawn weapon hangs off WEAPON or
        // SHIELD, which are attachment nodes rather than skinned bones, so they appear here and nowhere else.
        PosedNode* const cJoints[3] = {&chain.UpperArm, &chain.Forearm, &chain.Hand};
        std::vector<PosedNode>* const cLists[3] = {&chain.UpperSubtree, &chain.ForeSubtree, &chain.HandSubtree};

        for (size_t joint = 0; joint < 3; ++joint)
        {
            std::vector<NiAVObject*> found;
            CollectNodeSubtree(cJoints[joint]->pNode, 0, found);

            for (NiAVObject* pNode : found)
            {
                // A joint reached through the child tree is posed in its own right further down, so it must not
                // also be carried as somebody's descendant.
                if (pNode == chain.UpperArm.pNode || pNode == chain.Forearm.pNode || pNode == chain.Hand.pNode)
                    continue;

                AddUnique(*cLists[joint], PosedNode{pNode, FindBoneEntry(pBoneArray, cBoneCount, pNode)});
            }
        }

        /**
         * The outer lists have to contain the inner ones, *including the joints themselves*.
         *
         * Each joint is posed by taking its live transform, working out the delta to where it should be, and
         * applying that delta to everything in its list. That only composes correctly if a joint has already
         * been carried by its parent before its own delta is measured. Leaving the forearm and hand out of the
         * shoulder's list means the shoulder moves the fingers but not the two joints between, so the forearm's
         * delta is then computed as though the shoulder had never moved and lands on fingers that have already
         * been carried once. The palm ends up with one transform and the fingers with three, which is exactly a
         * hand whose fingers drift away from it.
         *
         * The probe got this right by accident: its node tree walk put the forearm and hand in the shoulder's
         * subtree for free. Collecting from the bone array skips them, so they have to be put back.
         */
        // Deduplicated, because a child tree walk from the forearm already descends through the hand, so the
        // two sources overlap. Carrying a bone twice applies the joint's delta to it twice.
        AddUnique(chain.ForeSubtree, chain.Hand);
        for (const PosedNode& cNode : chain.HandSubtree)
            AddUnique(chain.ForeSubtree, cNode);

        AddUnique(chain.UpperSubtree, chain.Forearm);
        for (const PosedNode& cNode : chain.ForeSubtree)
            AddUnique(chain.UpperSubtree, cNode);
    }

    // Prove the array layout against live data before anything is written through it. Each bone's stored world
    // translate must already agree with its node's, since nothing has touched either yet.
    size_t checked = 0;
    size_t agreed = 0;

    for (const ArmChain& cChain : aHands.Chain)
    {
        for (const PosedNode* pTarget : {&cChain.UpperArm, &cChain.Forearm, &cChain.Hand})
        {
            if (!pTarget->pNode || !pTarget->pFlatEntry)
                continue;

            ++checked;

            const auto* pStored = reinterpret_cast<const float*>(pTarget->pFlatEntry + kBoneEntryWorld + 0x24);

            if (glm::length(glm::vec3(pStored[0], pStored[1], pStored[2]) - ReadNodeWorld(pTarget->pNode).Translate) < 0.05f)
                ++agreed;
        }
    }

    aHands.LayoutConfirmed = checked > 0 && agreed == checked;

    if (!aHands.LayoutConfirmed)
    {
        spdlog::error("Flattened bone array layout did not validate for actor {:X}, so its hands will not be posed.", aHands.FormId);
        return false;
    }

    // A reference orientation per bone, relative to the actor's root so it survives the actor turning.
    // Composing onto the live rotation instead inherits the animation's roll and makes the arms spin.
    const glm::mat3 cRootInverse = glm::transpose(ReadNodeWorld(pRoot).Rotate);

    for (size_t hand = 0; hand < 2; ++hand)
    {
        const Xform cUpper = ReadPosed(aHands.Chain[hand].UpperArm);
        const Xform cFore = ReadPosed(aHands.Chain[hand].Forearm);
        const glm::vec3 cWrist = ReadPosed(aHands.Chain[hand].Hand).Translate;

        aHands.RestRotate[hand][0] = cRootInverse * cUpper.Rotate;
        aHands.RestRotate[hand][1] = cRootInverse * cFore.Rotate;
        aHands.RestDir[hand][0] = cRootInverse * (cFore.Translate - cUpper.Translate);
        aHands.RestDir[hand][1] = cRootInverse * (cWrist - cFore.Translate);
    }

    aHands.RestCaptured = true;

    // Only the node is found here. Its height is measured later, once the actor is in a pose worth measuring,
    // because a resolve can happen at a moment when the skeleton is not standing.
    aHands.pHead = FindByName(pRoot, "NPC Head [Head]");
    aHands.HeadHeight = 0.f;

    // The attachment nodes are worth naming, because a weapon or shield that does not follow the hand is
    // indistinguishable on screen from one whose node is not being carried. Bones are only counted.
    bool carriesWeapon = false;
    bool carriesShield = false;

    for (const PosedNode& cNode : aHands.Chain[1].UpperSubtree)
    {
        char name[128]{};

        if (CopyNodeName(cNode.pNode, name, sizeof(name)) && strcmp(name, "WEAPON") == 0)
            carriesWeapon = true;
    }

    for (const PosedNode& cNode : aHands.Chain[0].UpperSubtree)
    {
        char name[128]{};

        if (CopyNodeName(cNode.pNode, name, sizeof(name)) && strcmp(name, "SHIELD") == 0)
            carriesShield = true;
    }

    spdlog::info("Hand sync resolved actor {:X}: {} bones below the right shoulder, {} below the wrist; WEAPON {}, SHIELD {}", aHands.FormId, aHands.Chain[1].UpperSubtree.size(), aHands.Chain[1].HandSubtree.size(), carriesWeapon ? "carried" : "NOT CARRIED", carriesShield ? "carried" : "NOT CARRIED");

    // Record what every cached pointer points at, so a later rebuild that reuses the same addresses can be
    // told apart from the 3D this resolve actually saw.
    AuditPosedNodes(aHands, true);

    return true;
}

size_t HandPoseService::AuditPosedNodes(RemoteHands& aHands, const bool aCapture) noexcept
{
    size_t stale = 0;

    for (ArmChain& chain : aHands.Chain)
    {
        for (std::vector<PosedNode>* pList : {&chain.UpperSubtree, &chain.ForeSubtree, &chain.HandSubtree})
        {
            for (PosedNode& node : *pList)
                stale += AuditOnePosedNode(node, aCapture) ? 0 : 1;
        }

        for (PosedNode* pJoint : {&chain.UpperArm, &chain.Forearm, &chain.Hand})
            stale += AuditOnePosedNode(*pJoint, aCapture) ? 0 : 1;
    }

    return aCapture ? 0 : stale;
}

void HandPoseService::PoseActor(RemoteHands& aHands) noexcept
{
    auto* pActor = Cast<Actor>(TESForm::GetById(aHands.FormId));
    if (!pActor)
        return;

    NiNode* pRoot = pActor->GetNiNode();
    if (!pRoot)
        return;

    /**
     * A dead or dying actor's skeleton belongs to the ragdoll, not to us.
     *
     * On death Havok takes the bones over and drives them from the physics solver, and the 3D is rebuilt around
     * that. Writing arm transforms into it then fights a system that is itself writing every frame, through
     * pointers that the rebuild has probably already invalidated. This was on the list of guards from the start
     * and had not been written yet; a client crashed inside the game's own code moments after a kill.
     */
    if (pActor->IsDead() || pActor->actorState.IsBleedingOut())
        return;

    // Nothing behind the viewer gets posed, since the point is to spend nothing on arms nobody can see. The
    // forward axis this relies on was confirmed by watching the check flip as the viewer turned, so it no
    // longer logs.
    if (!IsInView(ReadNodeWorld(pRoot).Translate))
        return;


    // Re-resolve when the actor's 3D has been rebuilt under us, which the root pointer changing is the cheap
    // way to notice. Without this every cached bone node and array slot dangles and the writes below land in
    // freed memory.
    if (pRoot != aHands.Root || aHands.ChainFor != aHands.FormId || !aHands.RestCaptured)
    {
        // A failed resolve is not retried on the next frame. It is expensive enough to cost visible frame
        // time, and an actor whose 3D is not ready yet fails every frame until it is.
        if (aHands.ResolveFailed && aHands.SinceFailedResolve < kResolveRetryInterval)
            return;

        aHands.Root = pRoot;
        aHands.ChainFor = aHands.FormId;

        if (!ResolveChains(aHands, pActor))
        {
            // Logged because it used to fail in silence, and the only symptom was the frame time it cost.
            if (!aHands.ResolveFailed)
                spdlog::warn("Hand sync: could not resolve arms for actor {:X}, retrying every {:.0f}s. Its hands will not move until it does.", aHands.FormId, kResolveRetryInterval);

            aHands.ResolveFailed = true;
            aHands.SinceFailedResolve = 0.0;

            return;
        }

        if (aHands.ResolveFailed)
            spdlog::info("Hand sync: arms for actor {:X} resolved after an earlier failure", aHands.FormId);

        aHands.ResolveFailed = false;
    }

    /**
     * The root compare above is not sufficient on its own.
     *
     * Skyrim's node pools hand a freed block straight back, so a rebuilt 3D can land on the same address and
     * the compare passes with every child pointer dangling. It is also a check made here while Set3D frees the
     * old 3D on the game thread, so the answer can go stale between the check and the writes below.
     *
     * Verifying each cached pointer still points at the same object catches both. On 2026-08-18 it did not
     * exist and a write went into a BSLightingShaderProperty that had taken over a freed bone node's memory:
     * the world transform landed at +0x7C, the render pass list head at +0x98 took two floats of the rotation
     * matrix, and the game died in ClearRenderPassArrays walking a list whose head was 0.0066f.
     */
    if (const size_t cStale = AuditPosedNodes(aHands, false); cStale != 0)
    {
        spdlog::critical("Hand sync: {} cached bone pointers for actor {:X} no longer point at the objects they were resolved from, so its 3D was rebuilt under us. Skipping the pose and re-resolving rather than writing into whatever owns that memory now.", cStale, aHands.FormId);

        // Forces the resolve branch above on the next frame.
        aHands.Root = nullptr;

        return;
    }

    const Xform cRoot = ReadNodeWorld(pRoot);

    // The sneak bit was measured on SkyrimVR and nowhere else, so it is only read on a VR build. An SE client
    // reading a mask that was never checked against SE would offset hands on a guess; no crouch adjustment at
    // all is the smaller error. Re-measure the bit before turning this on for SE.
#if TP_SKYRIMVR
    const bool cSneaking = pActor->actorState.IsSneaking();
#else
    constexpr bool cSneaking = false;
#endif

    for (size_t hand = 0; hand < 2; ++hand)
    {
        const ArmChain& cChain = aHands.Chain[hand];

        if (!cChain.HasCore())
            continue;

        /**
         * The palm arrives relative to the sender's root, which is also the frame the arm has to look natural
         * in, so the two shaping rules are applied here rather than after it becomes a world position.
         *
         * Skyrim's axes on a character are X right, Y forward, Z up.
         */
        glm::vec3 palm = aHands.Palm[hand];

        /**
         * Rescale the whole reach from the sender's body to this character's.
         *
         * The palms are a real world measurement taken from a headset, and the character is not the player. A
         * female Skyrim character stands well short of a male one, so the same hand height lands proportionally
         * higher up her body: two players reaching to shake hands find one at chest height and the other at head
         * height, about thirty centimetres apart.
         *
         * Scaling by head height against the sender's eye height makes the pose a proportion of the body rather
         * than a distance. Uniform rather than vertical only, because a smaller body has shorter arms too, and
         * scaling only the height would leave the reach wrong instead.
         *
         * Both heights have to be sane before this is trusted. An older client sends no eye height at all, and
         * a scale of one is the honest answer there.
         */
        const bool cCanScale = aHands.SenderEyeHeight > 1.f && aHands.HeadHeight > 1.f;
        const float cScale = cCanScale ? aHands.HeadHeight / aHands.SenderEyeHeight : 1.f;

        palm *= cScale;

        // Out from the centre line, left hand to its left and right to its right. A headset reports hands where
        // they physically are, which is closer to the chest than a Skyrim arm wants to sit, and a goal that
        // near the body forces the elbow into a hard bend to reach it.
        //
        // After the rescale, so ten centimetres means ten centimetres on the character rather than on the player.
        palm.x += (hand == 0 ? -1.f : 1.f) * kOutwardOffset;

        // Crouching moves the body but not the tracked hands, so they are brought down to meet it. Also in
        // character units, and after the rescale, for the same reason.
        if (cSneaking)
        {
            palm.z -= kCrouchDrop;
            palm.x += kCrouchRightShift;

            if (hand == 0)
                palm.y += kCrouchLeftForward;
        }

        // Never behind the body. Reaching past the torso puts the goal somewhere the shoulder cannot get to
        // without folding the arm backwards through it, and the two bone solve will happily produce exactly
        // that. Clamping at the body plane costs a little reach and avoids every one of those poses.
        palm.y = std::max(palm.y, kMinForwardOffset);

        const glm::vec3 cGoal = cRoot.Translate + cRoot.Rotate * palm;

        const glm::vec3 cShoulder = ReadPosed(cChain.UpperArm).Translate;

        /**
         * Measure the head once it reads like a head.
         *
         * A head bone sits well above the shoulder on any standing skeleton, so that is the test. A reading
         * that fails it came from an actor that was not standing when it was taken, and accepting it fixes a
         * wrong scale in place for the rest of the session.
         */
        if (aHands.HeadHeight <= 0.f && aHands.pHead && IsReadable(aHands.pHead, kNiAVObjectSize))
        {
            const glm::mat3 cRootInv = glm::transpose(cRoot.Rotate);

            const float cHead = (cRootInv * (ReadNodeWorld(aHands.pHead).Translate - cRoot.Translate)).z;
            const float cShoulderHeight = (cRootInv * (cShoulder - cRoot.Translate)).z;

            if (cHead > cShoulderHeight + kMinHeadAboveShoulder)
            {
                aHands.HeadHeight = cHead;

                spdlog::info("Hand sync: actor {:X} head height {:.1f}, shoulder {:.1f}. Palms will be scaled to this body.", aHands.FormId, cHead, cShoulderHeight);
            }
        }
        const glm::vec3 cElbow = ReadPosed(cChain.Forearm).Translate;
        const glm::vec3 cWrist = ReadPosed(cChain.Hand).Translate;

        const float cUpperLen = glm::length(cElbow - cShoulder);
        const float cLowerLen = glm::length(cWrist - cElbow);

        const glm::vec3 cToTarget = cGoal - cShoulder;
        const float cRawDistance = glm::length(cToTarget);

        if (cRawDistance < 0.0001f || cUpperLen < 0.0001f || cLowerLen < 0.0001f)
            continue;

        // Clamped to what the arm can span, just short of straight so the bend plane stays defined. Without
        // this the hand sits beyond where the forearm ends and the mesh stretches across the gap.
        const float cMin = std::abs(cUpperLen - cLowerLen) + 0.01f;
        const float cMax = cUpperLen + cLowerLen - 0.01f;
        const float cDistance = glm::clamp(cRawDistance, cMin, cMax);

        const glm::vec3 cAxis = cToTarget / cRawDistance;
        const glm::vec3 cHandGoal = cShoulder + cAxis * cDistance;

        // Two bone analytic solve: where the elbow projects onto the shoulder to goal line, and how far off it.
        const float cAlong = (cUpperLen * cUpperLen - cLowerLen * cLowerLen + cDistance * cDistance) / (2.f * cDistance);
        const float cOff = std::sqrt(std::max(0.f, cUpperLen * cUpperLen - cAlong * cAlong));

        // Keep the existing bend plane so the elbow does not flip between frames.
        glm::vec3 hint = cElbow - cShoulder;
        hint = hint - cAxis * glm::dot(hint, cAxis);

        if (glm::length(hint) < 0.0001f)
        {
            const glm::vec3 cReference = std::abs(cAxis.z) < 0.9f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
            hint = cReference - cAxis * glm::dot(cReference, cAxis);
        }

        if (glm::length(hint) < 0.0001f)
            continue;

        hint = glm::normalize(hint);

        /**
         * Keep the elbow from swinging behind the body.
         *
         * The elbow cannot simply be moved: the two bone lengths pin it to a circle around the shoulder to goal
         * line, and a point off that circle is an arm that does not join up. What can be chosen is where on the
         * circle it sits, which is exactly what the hint decides, so the constraint goes here.
         *
         * The hint is taken into the character's own frame, its backward lean limited, then put back and made
         * perpendicular to the arm again. Inheriting the animation's bend plane is what lets an idle pose, whose
         * elbows point down and back, throw the elbow behind the torso once the hand is pulled forward.
         *
         * A little backward lean is left alone because a real elbow has one. Only past the limit is it pulled in.
         */
        glm::vec3 hintLocal = glm::transpose(cRoot.Rotate) * hint;

        if (hintLocal.y < kElbowBackwardLimit)
        {
            hintLocal.y = kElbowBackwardLimit;

            glm::vec3 corrected = cRoot.Rotate * hintLocal;

            // Back onto the plane of valid elbows, which the clamp will have nudged it off.
            corrected = corrected - cAxis * glm::dot(corrected, cAxis);

            if (glm::length(corrected) > 0.0001f)
                hint = glm::normalize(corrected);
        }

        const glm::vec3 cSolvedElbow = cShoulder + cAxis * cAlong + hint * cOff;

        const glm::mat3 cBaseUpper = cRoot.Rotate * aHands.RestRotate[hand][0];
        const glm::mat3 cBaseFore = cRoot.Rotate * aHands.RestRotate[hand][1];
        const glm::vec3 cBaseUpperDir = cRoot.Rotate * aHands.RestDir[hand][0];
        const glm::vec3 cBaseForeDir = cRoot.Rotate * aHands.RestDir[hand][1];

        const glm::mat3 cTurnUpper = RotationBetween(cBaseUpperDir, cSolvedElbow - cShoulder);

        Xform newUpper{};
        newUpper.Rotate = cTurnUpper * cBaseUpper;
        newUpper.Translate = cShoulder;

        // The shoulder carries the forearm with it, so the forearm's reference direction is turned first.
        const glm::mat3 cTurnFore = RotationBetween(cTurnUpper * cBaseForeDir, cHandGoal - cSolvedElbow);

        Xform newFore{};
        newFore.Rotate = cTurnFore * (cTurnUpper * cBaseFore);
        newFore.Translate = cSolvedElbow;

        PoseJoint(cChain.UpperArm, newUpper, cChain.UpperSubtree);
        PoseJoint(cChain.Forearm, newFore, cChain.ForeSubtree);

        // The wrist keeps whatever the forearm handed it. A wand node and a hand bone do not share a rest
        // frame, so writing the sender's palm orientation here would twist the palm off the wrist.
        Xform newHand{};
        newHand.Rotate = ReadPosed(cChain.Hand).Rotate;
        newHand.Translate = cHandGoal;

        PoseJoint(cChain.Hand, newHand, cChain.HandSubtree);
    }
}
