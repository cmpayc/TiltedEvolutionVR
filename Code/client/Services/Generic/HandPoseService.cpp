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

/**
 * @brief Half the cone treated as visible, in radians, measured from the headset's forward axis.
 *
 * Fifty degrees, which is inside the field of view of every headset this runs on, so anything the test keeps
 * is being drawn. It used to reject only what was clearly behind the viewer, on the reasoning that freezing
 * a visible player's arms was the worse artefact. It is not: posing an actor the renderer has culled tears
 * its skin into black strips, reported at ninety degrees of head turn, where the old test still posed.
 *
 * Tune this rather than the test if a headset turns out to see wider than fifty degrees off axis.
 */
constexpr float kViewHalfAngle = 50.f * 3.14159265f / 180.f;

/**
 * @brief Radius of the sphere the actor is treated as, in units, centred on its chest.
 *
 * A hundred units covers a body from the ground to above the head and both arms at full reach, so a sphere
 * this size is inside the view whenever any part of the actor could be. Being generous here is the safe
 * direction: it keeps posing somebody who is partly on screen, and a partly visible actor is being drawn,
 * which is the case that must not be cut.
 */
constexpr float kBodyRadius = 100.f;

// Chest height above the root when the actor's own head height has not been measured yet. Three quarters of
// a typical head height, and only ever a starting point: HeadHeight replaces it as soon as it is trusted.
constexpr float kDefaultChestHeight = 90.f;

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

/**
 * @brief Removed: each hand used to be pushed ten centimetres out from the body's centre line.
 *
 * It was there to keep a goal near the chest from forcing the elbow into a hard bend. What it also did was
 * separate the two hands by twenty centimetres, permanently, whatever the player did with them. Two palms
 * pressed together could not read as touching, which is worth more than a comfortable elbow.
 *
 * Left as a note rather than deleted because the elbow it was protecting is a real problem: if hands held near
 * the chest bend badly, this is what used to hide it, and the fix belongs in the solve rather than in a constant
 * that moves the goal.
 */

// How far forward of the body's centre a hand is allowed to get. Zero is the plane through the middle of the
// torso, which still leaves a hand inside the chest: a torso is a good fifteen centimetres deep from its centre
// to its front. Clamping in front of that keeps hands off the body rather than merely out of the back.
constexpr float kMinForwardOffset = 0.18f * kUnitsPerMetre;

/**
 * @brief The character's arm against the player's, for a shoulder relative palm.
 *
 * Measured on 2026-08-24, from the shoulder, which is the only place this can be measured honestly: a relaxed arm
 * hanging at the side spans 33.3 units while the character's arm is 38.9. So the character's arms are about
 * fifteen percent longer than the player's, and a 38.9 arm reaching 33.3 puts the elbow nearly ten units off the
 * straight line. That is a visible bend in the pose a player spends most of their time in.
 *
 * This is the same size difference the eye height ratio used to chase and never got right. That ratio divided by a
 * live headset height, which moves when the player leans or crouches: the same session logged 106, 109, 111 and
 * 131.9, so it crossed one and changed sign. An arm span does not move when somebody leans.
 *
 * A constant rather than a calibration. Learning it from the largest span seen does not work: VRIK stretches the
 * sender's own arm past the skeleton when the controller is out of reach, and this log has spans of 46 to 48
 * units on a 38.9 arm, so the maximum says nothing about how long the player's arm is.
 *
 * This is the knob. Raise it if relaxed arms still read bent, lower it if they lock straight too early.
 *
 * Raised from 1.15 after looking. At 1.15 an arm hanging straight down came out straight, but the pose a player
 * actually stands in, hands a little way in front, sits at 29 to 33 units from the shoulder and was still bending
 * eight to ten units. Measured on those same samples, 1.25 takes 29.2 to 36.5, leaving under seven, and carries
 * 31.8 and 32.9 past the arm's length so they clamp straight.
 *
 * There is a ceiling here worth knowing about. The elbow's offset from straight is fixed by the shoulder to hand
 * distance and nothing else, so the only way to straighten a closer pose is to push the hand further out, and
 * past about 1.3 that means straightening arms whose real counterpart genuinely is bent. The cost also grows:
 * the palm sits a fixed offset past the wrist and that offset does not scale, so two palms held together part by
 * (scale - 1) times the wrist separation, which at 1.25 is three or four centimetres.
 *
 * If that ceiling is reached, the way out is not a bigger number here. Shrinking the bone lengths the solve uses
 * straightens the elbow without moving the hand at all, which costs nothing in palm accuracy and makes the arm
 * look shorter instead. That is the right trade if this one runs out.
 */
constexpr float kArmReachScale = 1.25f;

/**
 * @brief Wrist separations over which the reach scale fades out, in units.
 *
 * The scale moves wrists, and the palm sits a fixed offset beyond the wrist that does not move with it, so two
 * palms held together part by (scale - 1) times the distance between the wrists. Pressed together the wrists are
 * about twelve units apart, which at 1.25 is three units of gap where there should be none.
 *
 * Fading the scale out as the hands close on each other costs nothing, because a reach correction is not wanted
 * there in the first place: hands together at the chest is a genuinely bent arm on a real body too. The pose that
 * could have been broken by this, hands together but arms extended, is safe without any scaling at all - a hand on
 * the centre line thirty five units forward is already 39.4 from a shoulder eighteen units to the side, past the
 * 38.9 arm, so it clamps straight on its own.
 *
 * One scale for both hands, always. Two hands scaled by different amounts separate however carefully each is
 * computed.
 */
constexpr float kPalmTogetherNear = 15.f;
constexpr float kPalmTogetherFar = 35.f;


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

constexpr const char* kNeckBone = "NPC Neck [Neck]";
constexpr const char* kHeadBone = "NPC Head [Head]";

/**
 * @brief How much of the head's turn the neck takes, as a fraction.
 *
 * A neck is not a hinge with a head on the end. The turn is spread along it, so giving the head bone all of it
 * folds the whole rotation into one joint and reads as a head sitting wrong on its shoulders, worst when
 * looking down, which is the direction a body cannot help with at all.
 *
 * A third is roughly what a real neck does and, more usefully, it is a number that cannot be got badly wrong:
 * the head is still written to its exact received orientation whatever this is, so this only decides how the
 * bend gets there. Raise it if necks look stiff, lower it if the head appears to lag the look direction.
 */
constexpr float kNeckShare = 0.35f;

/**
 * @brief The most the head is turned away from the body, in radians.
 *
 * A neck does about eighty degrees each way and this is not really there to model that. It is there because the
 * received rotation is relative to a body whose own rotation is interpolated on this machine and lags the
 * sender's. When it lags, that lag arrives here as extra yaw, and without a limit a player turning on the spot
 * would be drawn with their head screwed round past their shoulder.
 */
constexpr float kMaxHeadTurn = 80.f * 3.14159265f / 180.f;

/**
 * @brief How clearly one of the headset node's axes has to point up before the axis check believes it.
 *
 * The check reads which way the node's own z points in the character's frame. Level and upright it is straight
 * up, so anything close to that settles it, but a player lying down or mid somersault reads no axis as up and
 * the check has to wait rather than guess. 0.8 is about 37 degrees of head tilt, which is a long way past
 * anything a standing player does and a long way short of the quarter turn the wrong answer would look like.
 */
constexpr float kHeadUpDominance = 0.8f;

// Palm positions go out at this rate while they are moving.
constexpr double kSendInterval = 1.0 / 30.0;

// Below this, a palm counts as not having moved and nothing is sent. A still player then costs nothing.
constexpr float kSendThreshold = 0.5f;

/**
 * @brief Above this dot product, a palm counts as not having turned.
 *
 * cos of three quarters of a degree, because the angle between two rotations is twice the angle between their
 * quaternions taken as four dimensional vectors, so this gates at a degree and a half.
 *
 * Rotation needs a gate of its own. A wrist turning in place moves the palm by almost nothing, so on the
 * translation threshold alone a rotating hand would go out only on the keep alive.
 */
constexpr float kSendRotateThreshold = 0.9999143f;

/**
 * @brief How close a hand bone has to sit to its wand before it is believed to be following it, in units.
 *
 * The sent orientation is read off the sender's own hand bones, and what drives those from the controllers is
 * VRIK, which is not required here. Without it they carry the third person animation instead: a perfectly valid
 * rotation that has nothing to do with where the player's hands are pointing, which is worse than sending none.
 *
 * Measuring the distance tests the property that actually matters and does not care which mod provides it. A
 * driven wrist sits a few units from its wand, the wand being in the palm and the bone at the wrist. Thirty
 * five centimetres is loose enough to leave that alone and tight enough that an animated arm only lands inside
 * it by coincidence.
 */
constexpr float kWristTrackedDistance = 25.f;

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

/**
 * @brief The part of a rotation that turns about one given axis, by swing twist decomposition.
 *
 * A quaternion's vector part projected onto the axis, kept with the original scalar part and renormalised, is
 * exactly the rotation about that axis; what is left over turns only about axes square to it. Verified on the
 * case that matters: for q = twist(axis) * swing(perpendicular), this returns the twist unchanged.
 *
 * Used to take a wrist's pronation off the wrist and give it to the forearm, which is the bone that pronates.
 */
glm::mat3 TwistAbout(const glm::mat3& acRotation, const glm::vec3& acAxis) noexcept
{
    const glm::quat cWhole = glm::quat_cast(acRotation);

    const glm::vec3 cProjected = acAxis * glm::dot(glm::vec3(cWhole.x, cWhole.y, cWhole.z), acAxis);

    const glm::quat cTwist(cWhole.w, cProjected.x, cProjected.y, cProjected.z);

    const float cLength = glm::length(cTwist);

    // Degenerate when the rotation is a half turn about an axis square to this one. There is no twist in that,
    // and normalising a zero quaternion would give a matrix that collapses the bone rather than leaving it be.
    if (cLength < 0.0001f)
        return glm::mat3(1.f);

    return glm::mat3_cast(cTwist / cLength);
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
/**
 * @brief The one thing hanging off an arm that must not be carried with it.
 *
 * A shield is strapped to the left forearm and stays there in every weapon state. Measured on 2026-08-25: the
 * SHIELD node sat 6.4 units from the left hand in every sample taken, on four different bodies, before and
 * after the equip cycle that was supposed to move it, with the weapon state forced sheathed. Nothing moves it
 * because nothing is meant to: that is where the game keeps a shield.
 *
 * What is supposed to move is the arm. On the owner's screen a sheathed shield hangs at the waist because the
 * idle animation has the arm hanging at the waist, and the shield follows. Posing that arm to a VR player's
 * controller, which rests around chest height, carries the shield up with it, and the result reads exactly as a
 * shield stuck to the hand. The same log has it at 84.1 above the root while posed and 68.9 four hundred
 * milliseconds after posing stopped, on one body that had not moved.
 *
 * So the arm is still posed and the shield is left where the animation put it, which is the waist. It stops
 * following the forearm, which is a real cost: a player who raises their shield arm no longer raises the
 * shield. That is the lesser of the two, since the arm rests at chest height for most of a session and a shield
 * floating at the chest is wrong the whole time rather than only while blocking.
 *
 * Only SHIELD. A drawn weapon hangs off WEAPON and has to follow the hand, and none of this runs while a weapon
 * is drawn anyway, since the sender stops sending hands then.
 */
bool IsUncarriedAttachNode(const NiAVObject* acpNode) noexcept
{
    return NodeNameIs(acpNode, "SHIELD");
}

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

        // Skipped along with everything under it, since the shield's own geometry hangs below this node.
        if (IsUncarriedAttachNode(pChild))
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

        spdlog::info("Hand sync: posing other players' arms and heads is now {}. Sending is unchanged.", m_posingEnabled ? "ON" : "OFF");

        // Leave nothing half posed. The animation re-poses the whole skeleton every frame, so simply not
        // writing hands the arms and the head straight back.
        if (!m_posingEnabled)
        {
            std::scoped_lock lock(m_remotesMutex);

            for (auto& [id, hands] : m_remotes)
            {
                hands.HasHands = false;
                hands.HasHead = false;
            }
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

    // The player's own hand bones, re-resolved when its 3D is rebuilt. See m_localHand for why the orientation
    // comes from these and not from the wand nodes.
    if (pRoot != m_localHandRoot)
    {
        m_localHandRoot = pRoot;
        m_wristTrackingLogged = false;

        for (size_t hand = 0; hand < 2; ++hand)
        {
            m_localHand[hand] = FindByName(pRoot, kHandBone[hand]);
            m_localShoulder[hand] = FindByName(pRoot, kUpperArmBone[hand]);
        }
    }

    glm::vec3 palm[2]{};
    glm::quat palmRotate[2]{glm::quat(1.f, 0.f, 0.f, 0.f), glm::quat(1.f, 0.f, 0.f, 0.f)};

    /**
     * Whether hands are going out at all, which starts as the weapon state and is cleared by anything that
     * stops a controller being read.
     *
     * Separate from cActive because those failures used to abandon the whole send. They cannot any more: the
     * head rides in the same message and is read from the headset, which has nothing to do with the controllers
     * and no reason to stop being sent when one of them cannot be found.
     */
    bool handsActive = cActive;

    // Cleared by any hand that fails, so one untracked wrist stops both being sent. Sending a real orientation
    // for one hand and an idle animation for the other is harder to read on screen than sending neither.
    bool rotationValid = cActive;

    // Negative until measured, so the log below can tell a bone it could not read from one that is simply too
    // far from its wand.
    float wristToWand[2]{-1.f, -1.f};
    glm::quat wandToWrist[2]{};

    for (size_t hand = 0; handsActive && hand < 2; ++hand)
    {
        const auto* pSlot = reinterpret_cast<const uint8_t*>(pPlayer) + kWandNodeOffset[hand];

        auto* pWand = *reinterpret_cast<NiAVObject* const*>(pSlot);

        if (!pWand)
        {
            handsActive = false;
            break;
        }

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

                handsActive = false;

                break;
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
            handsActive = false;

            break;
        }

        const Xform cWand = ReadNodeWorld(pWand);

        /**
         * The wrist, and the test for whether it is really being driven from the controller.
         *
         * Tested every send rather than once, because VRIK can be switched off at runtime and because a bone
         * pointer that survived the root compare can still have been handed to something else by the node
         * pools. The cost is one VirtualQuery and a subtraction.
         */
        NiAVObject* pHandBone = m_localHand[hand];

        const bool cReadable = pHandBone && IsReadable(pHandBone, kNiAVObjectSize);
        const Xform cHandBone = cReadable ? ReadNodeWorld(pHandBone) : Xform{};

        if (cReadable)
            wristToWand[hand] = glm::length(cHandBone.Translate - cWand.Translate);

        NiAVObject* pShoulder = m_localShoulder[hand];
        const bool cHasShoulder = pShoulder && IsReadable(pShoulder, kNiAVObjectSize);

        // The shoulder is needed for the position as well as nothing else, so a hand without one is not tracked
        // even if its wrist is: there is nowhere to measure the palm from.
        const bool cTracked = cReadable && cHasShoulder && wristToWand[hand] <= kWristTrackedDistance;

        if (!cTracked)
            rotationValid = false;

        /**
         * The wrist's position, not the wand's, whenever there is a tracked wrist to read it from.
         *
         * The receiver writes this onto its own hand bone, so what it needs is where the sender's hand bone is.
         * A wand node sits in the palm, a good few centimetres from the wrist and in a direction that turns with
         * the hand, so sending it put the receiver's wrist where the sender's controller was and left an error
         * that swung around as the hand rotated. Two palms held together came out touching with the palms down
         * and a foot apart with them up, because turning both hands over reversed the offset on both at once.
         *
         * Same reasoning as the rotation: read the bone the receiver writes, and there is no offset to know.
         *
         * The wand is still the fallback. It is the wrong point by a few centimetres, but it is the only tracked
         * position a client without VRIK has, and it does not depend on anything driving the hand bones.
         */
        /**
         * From the shoulder when the wrist is tracked, from the root when it is not.
         *
         * See m_localShoulder for why. In short: VRIK moves this player's visible body most of the way to the
         * headset and leaves the root behind, the receiver knows nothing about that and puts the shoulder at the
         * skeleton's own offset, so a root relative palm arrives with a reach that is wrong by however far the
         * player has drifted. Measured at up to twelve units on a thirty nine unit arm, which is an arm that
         * cannot straighten no matter what the goal is scaled by.
         *
         * The wand fallback stays on the root. It is a raw controller position with no matching shoulder to
         * measure from, and the receiver's shaping rules are written for a root relative palm.
         */
        palm[hand] = cRootInverse * ((cTracked ? cHandBone.Translate - ReadNodeWorld(pShoulder).Translate : cWand.Translate - cRoot.Translate));


        if (!cTracked)
            continue;

        palmRotate[hand] = glm::quat_cast(cRootInverse * cHandBone.Rotate);

        // Kept for the log below, and only for that.
        wandToWrist[hand] = glm::quat_cast(glm::transpose(cWand.Rotate) * cHandBone.Rotate);
    }

    // A hand that dropped out of the loop above never had its rotation measured, so it cannot be claimed as
    // tracked whatever the wrist test said about the other one.
    rotationValid = rotationValid && handsActive;

    if (handsActive && !m_wristTrackingLogged)
    {
        m_wristTrackingLogged = true;

        if (wristToWand[0] < 0.f || wristToWand[1] < 0.f)
        {
            spdlog::warn("Hand sync: the local player's own hand bones could not be read, so positions fall back to the wand nodes and no rotation is sent.");
        }
        else if (rotationValid)
        {
            spdlog::info("Hand sync: wrists sit {:.1f} and {:.1f} units from their wands, so both position and rotation come from the hand bones.", wristToWand[0], wristToWand[1]);

            /**
             * The one measurement a client without VRIK would need, logged while there is something to measure
             * it against.
             *
             * With this offset a wand orientation could be turned into a hand bone orientation directly, and
             * rotation would stop depending on VRIK at all. It cannot be derived on a client that has no driven
             * hand bone to compare against, which is exactly the client that needs it, so it has to come from a
             * run like this one. Both hands, because the two are mirrored rather than equal.
             */
            spdlog::info("Hand sync: wand to wrist offset, left w{:.4f} x{:.4f} y{:.4f} z{:.4f}, right w{:.4f} x{:.4f} y{:.4f} z{:.4f}", wandToWrist[0].w, wandToWrist[0].x, wandToWrist[0].y, wandToWrist[0].z, wandToWrist[1].w, wandToWrist[1].x, wandToWrist[1].y, wandToWrist[1].z);
        }
        else
        {
            spdlog::info("Hand sync: wrists sit {:.1f} and {:.1f} units from their wands, past the {:.0f} that counts as following them, so positions fall back to the wand nodes and no rotation is sent. Driving the hand bones from the controllers is VRIK's job, and VRIK is not required here.", wristToWand[0], wristToWand[1], kWristTrackedDistance);
        }
    }

    /**
     * The headset, which carries two unrelated things and is read once for both.
     *
     * How high it is above this player's feet, which is what made the palms fit a character of another size and
     * is now only used by the wand fallback path. And which way it points, which is where the player is looking.
     *
     * Read here rather than after the send is decided, because whether to send at all depends on it: a player
     * standing perfectly still turning to look at something moves no palm at all.
     */
    float eyeHeight = 0.f;
    glm::quat headRotate(1.f, 0.f, 0.f, 0.f);
    bool headValid = false;

    auto* pHmd = *reinterpret_cast<NiAVObject* const*>(reinterpret_cast<const uint8_t*>(pPlayer) + kHmdNodeOffset);

    if (pHmd && IsReadable(pHmd, kNiAVObjectSize))
    {
        const Xform cHmd = ReadNodeWorld(pHmd);

        eyeHeight = (cRootInverse * (cHmd.Translate - cRoot.Translate)).z;

        /**
         * The headset in the character's own frame, which is the whole message.
         *
         * Not a world direction. The receiver's copy of this player stands at an interpolated rotation that
         * never matches this one, and SkyrimVR turns the body with the headset in the first place, so a world
         * orientation would arrive at a body that had already been turned and count that yaw a second time.
         * Relative, the body keeps the yaw it already has and this carries what is left over.
         */
        const glm::mat3 cHeadLocal = cRootInverse * cHmd.Rotate;

        if (!m_hmdChecked)
            CheckHmdNode(pHmd, cHeadLocal);

        if (m_hmdValid)
        {
            headRotate = glm::quat_cast(cHeadLocal);
            headValid = true;
        }
    }

    // A player holding still costs nothing, but the state still has to be repeated occasionally so a receiver
    // that missed the packet turning hands off does not leave an actor's arms frozen indefinitely.
    m_sinceKeepAlive += kSendInterval;

    const bool cDue = m_sinceKeepAlive >= kKeepAliveInterval;
    const bool cChanged = !m_hasActiveState || cActive != m_wasActive;

    // A wrist can turn through its whole range without the palm moving far enough to trip kSendThreshold, so
    // rotation gets its own comparison. The dot is taken absolute because q and -q are the same rotation.
    const bool cTurned = rotationValid && m_hasSent &&
                         (std::abs(glm::dot(palmRotate[0], m_lastSentRotate[0])) < kSendRotateThreshold || std::abs(glm::dot(palmRotate[1], m_lastSentRotate[1])) < kSendRotateThreshold);

    // The head needs the same test as a wrist, and needs it more: a player can look all the way round without a
    // palm moving at all, and does so with a weapon drawn, when no palm is being sent in the first place.
    const bool cLooked = headValid && m_hasSent && std::abs(glm::dot(headRotate, m_lastSentHead)) < kSendRotateThreshold;

    if (!cDue && !cChanged && !cTurned && !cLooked && m_hasSent && glm::length(palm[0] - m_lastSent[0]) < kSendThreshold && glm::length(palm[1] - m_lastSent[1]) < kSendThreshold)
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

    RequestHandPose request{};
    request.Id = m_localServerId;
    request.EyeHeight = eyeHeight;
    request.LeftPalm = palm[0];
    request.RightPalm = palm[1];
    request.LeftPalmRotation = palmRotate[0];
    request.RightPalmRotation = palmRotate[1];
    request.HandsActive = handsActive;
    request.HandsRotationValid = rotationValid;
    request.HeadRotation = headRotate;
    request.HeadRotationValid = headValid;

    m_transport.Send(request);

    m_lastSent[0] = palm[0];
    m_lastSent[1] = palm[1];
    m_lastSentRotate[0] = palmRotate[0];
    m_lastSentRotate[1] = palmRotate[1];
    m_lastSentHead = headRotate;
    m_wasActive = cActive;
    m_hasActiveState = true;
    m_hasSent = true;
#endif
}

/**
 * @brief Confirms the headset node by its name and then by its axes, once.
 *
 * The name check is the wands' check: the offset was measured rather than documented, so the node it lands on
 * is confirmed rather than assumed.
 *
 * The axis check is this one's own, and it is the difference between head sync working and every remote player
 * staring at the sky. What goes on the wire is the headset's orientation in the character's own frame, and the
 * receiver puts it back on a head bone as a rotation away from that body's rest. That only holds if a level
 * headset facing along the body reads as no rotation, which is true when the node carries the game's axes, x
 * right, y forward, z up, and false by a quarter turn if it carries the runtime's, where up is y and forward is
 * negative z.
 *
 * Which it is can be read off a single frame: whichever of the node's own axes points up the character's own up
 * is its up axis. A player whose head is not upright reads neither, and that is the one case where the check
 * says nothing and waits for a frame that can answer it, rather than guessing and being wrong for the session.
 */
void HandPoseService::CheckHmdNode(const NiAVObject* acpHmd, const glm::mat3& acRootRelative) noexcept
{
    if (!NodeNameIs(acpHmd, kHmdNodeName))
    {
        spdlog::error("PlayerCharacter+0x{:03X} is not {} on this build, so head rotation is not sent. The offset needs re-measuring.", kHmdNodeOffset, kHmdNodeName);

        m_hmdChecked = true;
        m_hmdValid = false;

        return;
    }

    // Columns, because a column of a rotation is where that axis of the node lands in the frame it is measured
    // in. The z of each is how far up the body's own up it points.
    const float cOwnZUp = acRootRelative[2].z;
    const float cOwnYUp = acRootRelative[1].z;

    if (cOwnZUp >= kHeadUpDominance)
    {
        m_hmdChecked = true;
        m_hmdValid = true;

        return;
    }

    if (cOwnYUp >= kHeadUpDominance)
    {
        m_hmdChecked = true;
        m_hmdValid = false;

        // Everything needed to fix it, since a run that reaches here is the only place the numbers exist. The
        // fix is a constant rotation applied where cHeadLocal is built, taking these axes onto the game's.
        spdlog::error("Head sync: the headset node's up is its own y ({:.2f}), not its z, so it carries the runtime's axes and the frame this sends in is wrong by a quarter turn. Head rotation is off rather than wrong. Its axes in the body's frame are x({:.3f} {:.3f} {:.3f}) y({:.3f} {:.3f} {:.3f}) z({:.3f} {:.3f} {:.3f}).", cOwnYUp, acRootRelative[0].x, acRootRelative[0].y, acRootRelative[0].z, acRootRelative[1].x, acRootRelative[1].y, acRootRelative[1].z, acRootRelative[2].x, acRootRelative[2].y, acRootRelative[2].z);

        return;
    }

    // Neither axis is up, which is a head that is not upright rather than a node that is wrong. Left undecided
    // on purpose: the next frame with a level head answers it.
}

bool HandPoseService::IsInView(const glm::vec3& acWorldPosition, const float aRadius) noexcept
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

    // Close enough that the viewer is standing inside the sphere. No meaningful direction, and an actor that
    // near is on screen whichever way the head is pointing.
    if (cDistance <= aRadius)
        return true;

    // Skyrim's convention is X right, Y forward, Z up, so the second column of the rotation is the node's
    // forward axis. Verified by the log line in PoseActor flipping when the viewer turns around.
    const glm::vec3 cForward = glm::vec3(cHmd.Rotate[1]);

    /**
     * Sphere against cone, rather than a point against a threshold.
     *
     * The point version could not be made to work with a real field of view. Testing the actor's root means
     * testing its feet, and somebody standing a metre and a half in front of the viewer has their feet nearly
     * sixty degrees below the view axis, so any cone tight enough to cull a player at ninety degrees also
     * froze the arms of one standing right in front of you.
     *
     * Widening the cone by the angle the actor subtends fixes both ends at once. Far away it barely widens it
     * at all, so a player off to the side is cut. Close up it widens enormously, which is correct: an actor
     * you are nearly touching is on screen whatever the angle to any single point on it.
     */
    const float cOffAxis = std::acos(glm::clamp(glm::dot(cForward, cToTarget / cDistance), -1.f, 1.f));

    return cOffAxis < kViewHalfAngle + std::asin(aRadius / cDistance);
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
    hands.HasHands = acMessage.HandsActive;

    /**
     * The head is taken before the hand gate and independently of it.
     *
     * A player with a weapon drawn sends no hands, because the game's animations own the arms then, and there is
     * no equivalent reason to stop sending a head: nothing else is deciding where it points, and somebody
     * looking around with a sword out is exactly when another player wants to see where they are looking.
     */
    hands.HasHead = acMessage.HeadRotationValid;

    // Left alone when the sender has nothing to say, for the reason the palm rotations are: the stored value
    // stays a rotation rather than becoming whatever the message's default was.
    if (acMessage.HeadRotationValid)
        hands.HeadRotate = acMessage.HeadRotation;

    if (!acMessage.HandsActive)
        return;

    hands.Palm[0] = acMessage.LeftPalm;
    hands.Palm[1] = acMessage.RightPalm;
    hands.SenderEyeHeight = acMessage.EyeHeight;

    hands.HasRotation = acMessage.HandsRotationValid;

    // Left alone when the sender has nothing tracked to send, so the stored pair stays a valid rotation rather
    // than becoming whatever the message's default happened to be.
    if (acMessage.HandsRotationValid)
    {
        hands.PalmRotate[0] = acMessage.LeftPalmRotation;
        hands.PalmRotate[1] = acMessage.RightPalmRotation;
    }
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
        {
            hands.HasHands = false;
            hands.HasHead = false;
        }

        if (hands.HasHands || hands.HasHead)
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
    aHands.Look = LookChain{};
    aHands.LayoutConfirmed = false;

    // Cleared up front so a resolve that fails half way cannot leave the previous run's flag standing, which
    // would stop the retry from ever happening and leave the arms with empty chains for good.
    aHands.RestCaptured = false;

    NiAVObject* pTree = FindByRttiName(pRoot, "BSFlattenedBoneTree", 0);
    uint8_t* pBoneArray = pTree ? GetBoneArray(pTree) : nullptr;
    const size_t cBoneCount = FindBoneCount(pTree, pBoneArray);

    if (!cBoneCount)
        return false;

    // Which of the four int16s at an entry+0x68 is the parent, decided from the arms below and then reused by
    // the head pass. It is a property of the array's layout rather than of any one bone, so working it out
    // again from a second pair of bones would only be a way of getting a different answer.
    int parentField = -1;

    /**
     * The shield's attach node in the bone array, so nothing at or below it is carried with the arm.
     *
     * Pruning it out of the child tree walk is not enough on its own, which the log of 2026-08-25 said plainly:
     * a body resolved after that pruning still reported the node as carried. The two sources are independent,
     * and this one reaches the same node by walking parent indices rather than children, so it never saw the
     * name check at all.
     *
     * See IsUncarriedAttachNode for why a shield must stay where the animation puts it.
     */
    NiAVObject* const cpShieldNode = FindByName(pRoot, "SHIELD");
    uint8_t* const cpShieldEntry = cpShieldNode ? FindBoneEntry(pBoneArray, cBoneCount, cpShieldNode) : nullptr;

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

        parentField = FindParentIndexField(pBoneArray, cBoneCount, chain.UpperArm.pFlatEntry, chain.Forearm.pFlatEntry, chain.Hand.pFlatEntry);

        if (parentField < 0)
            return false;

        for (size_t i = 0; i < cBoneCount; ++i)
        {
            uint8_t* pEntry = pBoneArray + i * kBoneEntryStride;

            if (pEntry == chain.UpperArm.pFlatEntry || pEntry == chain.Forearm.pFlatEntry || pEntry == chain.Hand.pFlatEntry)
                continue;

            // The shield's own node, reached through the array this time.
            if (cpShieldEntry && pEntry == cpShieldEntry)
                continue;

            std::vector<PosedNode>* pOwner = nullptr;
            int16_t walk = static_cast<int16_t>(i);

            for (size_t step = 0; step < 64; ++step)
            {
                walk = ReadBoneIndex(pBoneArray + static_cast<size_t>(walk) * kBoneEntryStride, static_cast<size_t>(parentField));

                if (walk < 0 || static_cast<size_t>(walk) >= cBoneCount)
                    break;

                uint8_t* pAncestor = pBoneArray + static_cast<size_t>(walk) * kBoneEntryStride;

                // Tested before the arm joints, because the shield node hangs off the forearm: anything below
                // it reaches the forearm as well, and whichever is found first decides. Leaving pOwner null
                // drops the bone from every list, which is the point.
                if (cpShieldEntry && pAncestor == cpShieldEntry)
                    break;

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

    /**
     * The neck and the head, collected the same two ways and for the same two reasons.
     *
     * The bone array is where the face is. Eyes, jaw and the rest of the facegen bones are flattened away and
     * have no node to find, so a head turned without them leaves a face behind in the air. The child tree is
     * where anything hung off the head is: a helmet, hair, a circlet. Neither source alone is the head.
     *
     * A missing neck is not a failure. It costs the shared bend and nothing else, so the head is still posed.
     */
    LookChain& look = aHands.Look;

    look.Neck.pNode = FindByName(pRoot, kNeckBone);
    look.Head.pNode = FindByName(pRoot, kHeadBone);

    if (look.Head.pNode)
    {
        look.Head.pFlatEntry = FindBoneEntry(pBoneArray, cBoneCount, look.Head.pNode);

        if (look.Neck.pNode)
            look.Neck.pFlatEntry = FindBoneEntry(pBoneArray, cBoneCount, look.Neck.pNode);

        for (size_t i = 0; i < cBoneCount; ++i)
        {
            uint8_t* pEntry = pBoneArray + i * kBoneEntryStride;

            if (pEntry == look.Head.pFlatEntry || pEntry == look.Neck.pFlatEntry)
                continue;

            std::vector<PosedNode>* pOwner = nullptr;
            int16_t walk = static_cast<int16_t>(i);

            for (size_t step = 0; step < 64; ++step)
            {
                walk = ReadBoneIndex(pBoneArray + static_cast<size_t>(walk) * kBoneEntryStride, static_cast<size_t>(parentField));

                if (walk < 0 || static_cast<size_t>(walk) >= cBoneCount)
                    break;

                uint8_t* pAncestor = pBoneArray + static_cast<size_t>(walk) * kBoneEntryStride;

                // The head first, because a bone below it is below the neck as well and the innermost owner is
                // the one whose motion it has to follow.
                if (pAncestor == look.Head.pFlatEntry)
                {
                    pOwner = &look.HeadSubtree;
                    break;
                }
                if (pAncestor == look.Neck.pFlatEntry)
                {
                    pOwner = &look.NeckSubtree;
                    break;
                }
            }

            if (!pOwner)
                continue;

            auto* pRefNode = *reinterpret_cast<NiAVObject* const*>(pEntry + kBoneEntryRefNode);

            AddUnique(*pOwner, PosedNode{IsReadable(pRefNode, kNiAVObjectSize) ? pRefNode : nullptr, pEntry});
        }

        std::vector<NiAVObject*> found;
        CollectNodeSubtree(look.Head.pNode, 0, found);

        for (NiAVObject* pNode : found)
            AddUnique(look.HeadSubtree, PosedNode{pNode, FindBoneEntry(pBoneArray, cBoneCount, pNode)});

        if (look.Neck.pNode)
        {
            found.clear();
            CollectNodeSubtree(look.Neck.pNode, 0, found);

            for (NiAVObject* pNode : found)
            {
                // The head is posed in its own right below, so it must not also be carried as a descendant
                // here. It goes into the neck's list as itself, once, with its array slot attached.
                if (pNode == look.Head.pNode)
                    continue;

                AddUnique(look.NeckSubtree, PosedNode{pNode, FindBoneEntry(pBoneArray, cBoneCount, pNode)});
            }
        }

        // The neck's list has to contain the head and everything under it, for the reason the shoulder's has to
        // contain the forearm: the head's own delta is measured after the neck has already carried it.
        AddUnique(look.NeckSubtree, look.Head);

        for (const PosedNode& cNode : look.HeadSubtree)
            AddUnique(look.NeckSubtree, cNode);
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
        const Xform cHand = ReadPosed(aHands.Chain[hand].Hand);

        aHands.RestRotate[hand][0] = cRootInverse * cUpper.Rotate;
        aHands.RestRotate[hand][1] = cRootInverse * cFore.Rotate;
        aHands.RestRotate[hand][2] = cRootInverse * cHand.Rotate;
        aHands.RestDir[hand][0] = cRootInverse * (cFore.Translate - cUpper.Translate);
        aHands.RestDir[hand][1] = cRootInverse * (cHand.Translate - cFore.Translate);
    }

    /**
     * The same for the neck and head, with the caveat that belongs to them alone.
     *
     * The game turns an actor's head by itself: headtracking has an NPC look at whoever is nearby, and if that
     * is happening now then this capture has that turn in it and every head pose after it is off by the same
     * amount, in yaw, for as long as the 3D lives. This is the first place to look if remote players end up
     * looking consistently to one side of where they should be, and the fix is the skin's bind pose, which is
     * the rest orientation by definition rather than by luck.
     */
    if (aHands.Look.Neck.pNode)
        aHands.RestNeckRotate = cRootInverse * ReadPosed(aHands.Look.Neck).Rotate;

    if (aHands.Look.Head.pNode)
        aHands.RestHeadRotate = cRootInverse * ReadPosed(aHands.Look.Head).Rotate;

    aHands.RestCaptured = true;

    // The head's height is measured later, once the actor is in a pose worth measuring, because a resolve can
    // happen at a moment when the skeleton is not standing.
    aHands.pHead = aHands.Look.Head.pNode;
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

    // The two verdicts mean opposite things now. A weapon has to follow the hand; a shield has to be left
    // behind, so it appearing here at all is the fault. See IsUncarriedAttachNode.
    spdlog::info("Hand sync resolved actor {:X}: {} bones below the right shoulder, {} below the wrist; WEAPON {}, SHIELD {}", aHands.FormId, aHands.Chain[1].UpperSubtree.size(), aHands.Chain[1].HandSubtree.size(), carriesWeapon ? "carried" : "NOT CARRIED", carriesShield ? "CARRIED, which will drag it off the waist" : "left at the waist");

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

    // The head's pointers come from the same 3D and dangle with it, so they are checked with the same rule.
    for (std::vector<PosedNode>* pList : {&aHands.Look.NeckSubtree, &aHands.Look.HeadSubtree})
    {
        for (PosedNode& node : *pList)
            stale += AuditOnePosedNode(node, aCapture) ? 0 : 1;
    }

    for (PosedNode* pJoint : {&aHands.Look.Neck, &aHands.Look.Head})
        stale += AuditOnePosedNode(*pJoint, aCapture) ? 0 : 1;

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

    /**
     * Nothing the renderer has culled gets posed.
     *
     * Two reasons, and the second is why this is a hard rule rather than an optimisation. Spending nothing on
     * arms nobody can see is the cheap one. The real one is that writing bone transforms into an actor the
     * renderer is not drawing tears its skin: the reported symptom was black strips across the view whenever
     * the other player moved their hands while off to one side.
     *
     * The chest rather than the root, because the root is the actor's feet and they are far below the view
     * axis at close range. See IsInView for why the sphere is what makes both ends of that work.
     */
    const float cChestHeight = aHands.HeadHeight > 0.f ? aHands.HeadHeight * 0.75f : kDefaultChestHeight;
    const glm::vec3 cChest = ReadNodeWorld(pRoot).Translate + glm::vec3(0.f, 0.f, cChestHeight);

    const bool cInView = IsInView(cChest, kBodyRadius);

    if (cInView != aHands.WasInView)
    {
        aHands.WasInView = cInView;

        // Edge triggered, so this is a handful of lines per session rather than one per frame. It is here to
        // be tuned against: if the strips come back, this says at what moment posing resumed or stopped.
        spdlog::info("Hand sync: actor {:X} {} view, so its arms {}", aHands.FormId, cInView ? "entered" : "left", cInView ? "follow the sender again" : "go back to the game's own animation");
    }

    if (!cInView)
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

    /**
     * The point the reach scale works about: the midpoint of the two shoulders.
     *
     * A single origin shared by both hands is the whole reason this is safe. Scaling each hand from its own
     * shoulder would separate two hands that are in the same place by (scale - 1) times the shoulder separation,
     * about eight centimetres, because the two hands would be pushed along different lines. Scaled about one
     * point, two coincident wrists stay coincident exactly.
     *
     * Both arms have to be resolved for there to be a midpoint. Without one nothing is scaled, which is the old
     * behaviour rather than a guess at where the chest is.
     */
    const bool cHaveShoulderMid = aHands.HasHands && aHands.Chain[0].HasCore() && aHands.Chain[1].HasCore();
    const glm::vec3 cShoulderMid = cHaveShoulderMid ? 0.5f * (ReadPosed(aHands.Chain[0].UpperArm).Translate + ReadPosed(aHands.Chain[1].UpperArm).Translate) : cRoot.Translate;

    /**
     * How much of the reach scale to use, faded out as the two hands close on each other. See kPalmTogetherNear.
     *
     * Both goals have to be known before either is scaled, so this is a pass of its own. It has to be one number
     * shared by both hands: two hands scaled by different amounts come apart no matter how each is worked out.
     */
    float reachScale = 1.f;

    if (cHaveShoulderMid && aHands.HasRotation)
    {
        const glm::vec3 cLeft = ReadPosed(aHands.Chain[0].UpperArm).Translate + cRoot.Rotate * aHands.Palm[0];
        const glm::vec3 cRight = ReadPosed(aHands.Chain[1].UpperArm).Translate + cRoot.Rotate * aHands.Palm[1];

        const float cApart = glm::length(cLeft - cRight);
        const float cFade = glm::clamp((cApart - kPalmTogetherNear) / (kPalmTogetherFar - kPalmTogetherNear), 0.f, 1.f);

        reachScale = 1.f + (kArmReachScale - 1.f) * cFade;
    }

    // Arms only while the sender is sending them. A weapon drawn stops the hands and leaves the head, so this
    // is now a loop that can be skipped with the rest of the pose still to write.
    for (size_t hand = 0; aHands.HasHands && hand < 2; ++hand)
    {
        const ArmChain& cChain = aHands.Chain[hand];

        if (!cChain.HasCore())
            continue;

        /**
         * The palm arrives relative to the sender's own shoulder when its wrist was tracked, and relative to its
         * root when it was not. Skyrim's axes on a character are X right, Y forward, Z up.
         *
         * The shoulder is the frame that makes the reach right, and it is the only one that can. The two machines
         * disagree about where a shoulder is: VRIK moves the sender's visible body about six tenths of the way to
         * its headset and leaves the 3D root on the game's reference, and the receiver knows nothing about that
         * and puts the shoulder at the skeleton's own offset from the root. Measured on 2026-08-24, a drift of
         * 20.2 units came with a head bone offset of 12.6, leaving the two shoulders up to twelve units apart on a
         * thirty nine unit arm. A root relative palm therefore arrives with a reach wrong by that much, in
         * whichever direction the player last stepped, which is an arm that will not straighten however the goal
         * is scaled. The reach diagnostic caught it as `raw 43.7, arm 38.9`: five units past full extension, on a
         * hand that was not extended.
         *
         * Measured shoulder to shoulder the drift cancels and the receiver reproduces the arm span the sender's
         * own arm actually had. Both shoulders shift by the same amount, so two palms held together stay together.
         *
         * The eye height rescale that used to sit here is gone with it. It was dividing by a live headset height,
         * which is not a body measurement at all: the same session logged 106, 109, 111 and 131.9 as the player
         * leaned and crouched, so the ratio crossed one and the reach correction changed sign. Shoulder relative
         * needs no size ratio, because it carries the arm span rather than a distance from the feet.
         */
        glm::vec3 palm = aHands.Palm[hand];

        const glm::vec3 cShoulder = ReadPosed(cChain.UpperArm).Translate;

        /**
         * A wand position still gets every rule it was written for: it is a real world measurement that has to be
         * fitted onto a body of another size and kept out of places an arm cannot go. A shoulder relative hand
         * bone position needs none of them. It already carries the sender's own arm span, and the rules that
         * resize or clamp it are the ones that were fighting the reach.
         */
        glm::vec3 goal{};

        if (aHands.HasRotation)
        {
            goal = cShoulder + cRoot.Rotate * palm;

            // Up to the character's proportions, about the shoulder midpoint so the two hands keep their
            // relationship to each other, and faded out where they touch. See kArmReachScale and
            // kPalmTogetherNear.
            if (cHaveShoulderMid)
                goal = cShoulderMid + reachScale * (goal - cShoulderMid);
        }
        else
        {
            const bool cCanScale = aHands.SenderEyeHeight > 1.f && aHands.HeadHeight > 1.f;

            palm *= cCanScale ? aHands.HeadHeight / aHands.SenderEyeHeight : 1.f;

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

            goal = cRoot.Translate + cRoot.Rotate * palm;
        }

        const glm::vec3 cGoal = goal;

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

        /**
         * No reach correction of any kind for a shoulder relative palm.
         *
         * There was a ramped radial extension here, built to make up an eleven percent size difference that the
         * eye height ratio reported. It was measuring the wrong thing twice over: the ratio came from a live
         * headset height that moved between 106 and 131.9 in one session, so it crossed one and changed sign, and
         * the shortfall it was compensating for was not a size difference at all but the shoulder disagreement
         * that shoulder relative palms now cancel. Both are gone rather than stacked on top of each other.
         */
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

        /**
         * The palm's own orientation, when the sender had one worth sending.
         *
         * This looks like it breaks the rule that a bone's rotation is never built from scratch, and it does
         * not. That rule is about aiming a fresh frame down a bone, which throws away the rest orientation and
         * the roll. What arrives here is the measured orientation of the same bone on the same skeleton, so
         * both are already in it. Only the frame has to be changed, from the sender's root to this one's.
         *
         * Scale free, so the eye height rescale that the palm position goes through does not apply.
         *
         * Computed before the forearm is posed, because the forearm's roll is derived from it below. The case
         * with no rotation still has to read the hand after the forearm has carried it, so that one is left
         * where it was.
         */
        const glm::mat3 cWantedHand = aHands.HasRotation ? cRoot.Rotate * glm::mat3_cast(aHands.PalmRotate[hand]) : glm::mat3(1.f);

        /**
         * Give the wrist's pronation to the forearm, which is the bone that pronates.
         *
         * Without this the forearm keeps very nearly its reference roll wherever the hand points, because both
         * turns that reach it are shortest rotations and a shortest rotation adds no roll of its own. A palm
         * turned right over then disagrees with its own forearm by most of a pronation, and since the skin
         * across the wrist is weighted to both, it shears: the hand reads as tied in a knot at the wrist.
         *
         * A real arm has no wrist joint that can do this. Pronation happens along the forearm, between the two
         * bones in it, which is also how Skyrim animates it, so that is where the rotation belongs.
         *
         * Three steps. Take the orientation the hand would have if the wrist were sitting at the pose the
         * reference was captured in. The rotation from there to the wanted orientation is the wrist's own
         * articulation. Its component about the forearm's length is the pronation, and moving that onto the
         * forearm leaves the wrist bending and deviating but no longer twisting.
         *
         * Rolling about the forearm's length does not move the hand: the wrist sits on that axis, so cHandGoal
         * and the solve behind it are untouched.
         */
        if (aHands.HasRotation)
        {
            const glm::vec3 cForeAxis = cHandGoal - cSolvedElbow;

            if (glm::length(cForeAxis) > 0.0001f)
            {
                const glm::mat3 cWristRest = glm::transpose(aHands.RestRotate[hand][1]) * aHands.RestRotate[hand][2];
                const glm::mat3 cUnarticulated = newFore.Rotate * cWristRest;

                newFore.Rotate = TwistAbout(cWantedHand * glm::transpose(cUnarticulated), glm::normalize(cForeAxis)) * newFore.Rotate;
            }
        }

        PoseJoint(cChain.UpperArm, newUpper, cChain.UpperSubtree);
        PoseJoint(cChain.Forearm, newFore, cChain.ForeSubtree);

        Xform newHand{};
        newHand.Rotate = aHands.HasRotation ? cWantedHand : ReadPosed(cChain.Hand).Rotate;
        newHand.Translate = cHandGoal;

        PoseJoint(cChain.Hand, newHand, cChain.HandSubtree);
    }

    // Last, and after the arms rather than before them, only because the head's rest was captured from the same
    // frame the arms' was and reading it back before either has been written keeps that true.
    if (aHands.HasHead)
        PoseLook(aHands, cRoot.Rotate);
}

/**
 * @brief Turns one actor's neck and head to where its player is looking.
 *
 * What arrives is the player's headset in their own body's frame, so it is already the thing that transfers: a
 * rotation away from facing straight ahead, which means the same on a body of any size and needs no goal, no
 * chain and no scaling. The receiving end is three steps.
 *
 * **Put it back on this body.** The rotation is composed onto this actor's own rest orientation for the bone,
 * captured relative to its root, rather than onto whatever the animation has the head doing this frame.
 * Composing onto the live pose would add the game's own headtracking to the player's, and a head being turned
 * by two things at once ends up looking at neither.
 *
 * **Limit it.** The body this is relative to is interpolated on this machine and lags the sender's, and that lag
 * arrives here as yaw the player never turned. Without a limit, a player spinning on the spot is drawn with
 * their head screwed round behind them.
 *
 * **Share it with the neck.** A head is not hinged to the shoulders. The neck takes a third and the head is
 * still written to the full received orientation, so the share decides how the bend is distributed and not
 * where the face ends up.
 */
void HandPoseService::PoseLook(RemoteHands& aHands, const glm::mat3& acRootRotate) noexcept
{
    LookChain& look = aHands.Look;

    if (!look.HasCore())
        return;

    /**
     * The turn away from straight ahead, limited.
     *
     * As an axis and an angle, because that is the only form where "less of the same rotation" is a single
     * multiplication. Normalised first: the quaternion came off the wire through a quantiser that renormalises
     * to within a step, and glm::angle on a quaternion whose w has drifted past one returns a NaN that would
     * spread through the matrix and into the actor's skeleton.
     */
    glm::quat turn = glm::normalize(aHands.HeadRotate);

    /**
     * Taken to the half with a positive scalar part first, which is not tidying up.
     *
     * q and -q are the same rotation, and glm::angle reports the second as the long way round the same axis,
     * past a half turn. Clamping that would rebuild the rotation from an axis pointing the other way, so a head
     * turned a little to the left would be redrawn turned a lot to the right.
     */
    if (turn.w < 0.f)
        turn = -turn;

    if (glm::angle(turn) > kMaxHeadTurn)
        turn = glm::angleAxis(kMaxHeadTurn, glm::axis(turn));

    const glm::mat3 cTurn = glm::mat3_cast(turn);

    /**
     * The neck first, so the head is carried by it before its own delta is measured.
     *
     * Its share is the same rotation taken part way, which is a slerp from no rotation at all. Interpolating the
     * matrix instead would not be a rotation on the way.
     */
    if (look.Neck.pNode || look.Neck.pFlatEntry)
    {
        const glm::mat3 cNeckTurn = glm::mat3_cast(glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), turn, kNeckShare));

        Xform newNeck{};
        newNeck.Rotate = acRootRotate * cNeckTurn * aHands.RestNeckRotate;
        newNeck.Translate = ReadPosed(look.Neck).Translate;

        PoseJoint(look.Neck, newNeck, look.NeckSubtree);
    }

    /**
     * Then the head, at the full turn.
     *
     * Its translation is read after the neck has moved rather than computed, because the neck rotating about a
     * point below the head is what puts the head where it belongs, and that has already happened by here.
     */
    Xform newHead{};
    newHead.Rotate = acRootRotate * cTurn * aHands.RestHeadRotate;
    newHead.Translate = ReadPosed(look.Head).Translate;

    PoseJoint(look.Head, newHead, look.HeadSubtree);
}
