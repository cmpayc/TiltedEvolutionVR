#pragma once

#include <Actor.h>
#include <Misc/TintMask.h>
#include <Forms/ActorValueInfo.h>

struct Skills
{
    enum Skill : std::uint32_t
    {
        kOneHanded = 0,
        kTwoHanded = 1,
        kArchery = 2,
        kBlock = 3,
        kSmithing = 4,
        kHeavyArmor = 5,
        kLightArmor = 6,
        kPickpocket = 7,
        kLockpicking = 8,
        kSneak = 9,
        kAlchemy = 10,
        kSpeech = 11,
        kAlteration = 12,
        kConjuration = 13,
        kDestruction = 14,
        kIllusion = 15,
        kRestoration = 16,
        kEnchanting = 17,
        kTotal
    };

    static const Skill GetSkillFromActorValue(int32_t aActorValue) noexcept
    {
        switch (aActorValue)
        {
        case ActorValueInfo::kOneHanded: return kOneHanded;
        case ActorValueInfo::kTwoHanded: return kTwoHanded;
        case ActorValueInfo::kMarksman: return kArchery;
        case ActorValueInfo::kBlock: return kBlock;
        case ActorValueInfo::kSmithing: return kSmithing;
        case ActorValueInfo::kHeavyArmor: return kHeavyArmor;
        case ActorValueInfo::kLightArmor: return kLightArmor;
        case ActorValueInfo::kPickpocket: return kPickpocket;
        case ActorValueInfo::kLockpicking: return kLockpicking;
        case ActorValueInfo::kSneak: return kSneak;
        case ActorValueInfo::kAlchemy: return kAlchemy;
        case ActorValueInfo::kSpeechcraft: return kSpeech;
        case ActorValueInfo::kAlteration: return kAlteration;
        case ActorValueInfo::kConjuration: return kConjuration;
        case ActorValueInfo::kDestruction: return kDestruction;
        case ActorValueInfo::kIllusion: return kIllusion;
        case ActorValueInfo::kRestoration: return kRestoration;
        case ActorValueInfo::kEnchanting: return kEnchanting;
        default: return kTotal;
        }
    }

    static const char* GetSkillString(Skill aSkill) noexcept
    {
        switch (aSkill)
        {
        case kOneHanded: return "One-handed";
        case kTwoHanded: return "Two-handed";
        case kArchery: return "Archery";
        case kBlock: return "Block";
        case kSmithing: return "Smithing";
        case kHeavyArmor: return "Heavy armor";
        case kLightArmor: return "Light armor";
        case kPickpocket: return "Pickpocket";
        case kLockpicking: return "Lockpicking";
        case kSneak: return "Sneak";
        case kAlchemy: return "Alchemy";
        case kSpeech: return "Speech";
        case kAlteration: return "Alteration";
        case kConjuration: return "Conjuration";
        case kDestruction: return "Destruction";
        case kIllusion: return "Illusion";
        case kRestoration: return "Restoration";
        case kEnchanting: return "Enchanting";
        default: return "UNKNOWN";
        }
    }

    struct SkillData
    {
        float level;
        float xp;
        float levelThreshold;
    };
    static_assert(sizeof(SkillData) == 0xC);

    float xp;
    float levelThreshold;
    SkillData skills[Skill::kTotal];
    uint32_t legendaryLevels[Skill::kTotal];
};

struct TESQuest;

// SkyrimVR reorganises PlayerCharacter rather than padding it, so no member's VR offset can be
// derived from another's. The five that are located prove that themselves, because their deltas all
// differ: 0x5E8 for objectives, 0x6F8 for pSkills and locationForm, 0x6F4 for difficulty and 0x6F0
// for baseTints. The evidence for each is at its member.
#if TP_SKYRIMVR
constexpr size_t kObjectivesOffset = 0xB70;
constexpr size_t kSkillsOffset = 0x10B0;
constexpr size_t kLocationFormOffset = 0x11C8;
constexpr size_t kDifficultyOffset = 0x11F4;
#else
constexpr size_t kObjectivesOffset = 0x588;
constexpr size_t kSkillsOffset = 0x9B8;
constexpr size_t kLocationFormOffset = 0xAD0;
constexpr size_t kDifficultyOffset = 0xB00;
#endif

struct PlayerCharacter : Actor
{
    static constexpr FormType Type = FormType::Character;
    static int32_t LastUsedCombatSkill;

    static PlayerCharacter* Get() noexcept;

    static void SetGodMode(bool aSet) noexcept;

    const GameArray<TintMask*>& GetTints() const noexcept;

    // TODO: there's an in game function for this in fallout 4, maybe also for skyrim?
    void SetDifficulty(const int32_t aDifficulty, bool aForceUpdate = true, bool aExpectGameDataLoaded = true) noexcept;

    void AddSkillExperience(int32_t aSkill, float aExperience) noexcept;
    float GetSkillExperience(Skills::Skill aSkill) const noexcept { return (*pSkills)->skills[aSkill].xp; }

    NiPoint3 RespawnPlayer() noexcept;

    void PayCrimeGoldToAllFactions() noexcept;

    void SetWaypoint(NiPoint3* apPosition, TESWorldSpace* apWorldSpace) noexcept;
    void RemoveWaypoint() noexcept;

    struct Objective
    {
        BSFixedString name;
        TESQuest* quest;
    };

    struct ObjectiveInstance
    {
        Objective* instance;
        uint64_t instanceCount;
    };

    uint8_t pad1[kObjectivesOffset - sizeof(Actor)];
    // SE 0x588, VR 0xB70. Proven twice over. At runtime, a scan of the player object found exactly
    // one GameArray at +0xB70 whose element 0 Objective leads to a form of type Quest (capacity 11,
    // length 11, quest 0x5000BD7), which is four dereferences no unrelated field survives. In the
    // binary, VR 0x1406CAA60 passes `lea rcx,[rdi+0xB70]` to three array helpers; that function is
    // the counterpart of SE 0x14073D850, the only SE function that iterates this array, confirmed by
    // sharing 21 of 22 mapped callees with it. Elements are 0x10 bytes on both, from the `shl rax,4`
    // SE uses to index it.
    GameArray<ObjectiveInstance> objectives;
    uint8_t padObjectives[kSkillsOffset - (kObjectivesOffset + sizeof(GameArray<ObjectiveInstance>))];
    // SE 0x9B8, VR 0x10B0. AddSkillExperience (id 40488, curated-verified) is byte for byte the same
    // function in both builds bar this displacement and its call target, and its whole body is the
    // load: SE 0x140736E20 `48 8b 89 b8 09 00 00` = `mov rcx,[rcx+0x9B8]`, VR 0x1406C30B0
    // `48 8b 89 b0 10 00 00` = `mov rcx,[rcx+0x10B0]`, in each case passing the member straight to
    // the PlayerSkills call that follows. Corroborated by the shape of the users: SE has a cluster
    // of `mov rcx,[rax+0x9B8]` through the Papyrus natives which VR reproduces at 0x10B0, while
    // VR's own 0x9B8 is touched only by stack frames.
    Skills** pSkills;
    uint8_t padSkills[kLocationFormOffset - (kSkillsOffset + sizeof(Skills**))];
    // SE 0xAD0, VR 0x11C8. TESObjectREFR::GetCurrentLocation (id 19812, curated-verified) is again
    // byte identical between the builds bar displacements: it tests `formID == 0x14`, which is the
    // player, then loads the player singleton and returns this member, SE 0x1402ED5EF
    // `mov rax,[rax+0xAD0]` against VR 0x1402AABFF `mov rax,[rax+0x11C8]`. The singleton each reads
    // has exactly two writers in its own image and the two sets pair one for one, including
    // SE 0x14064A90B / VR 0x1405BECE2, the write that follows the player's constructor call.
    TESForm* locationForm;
    uint8_t padLocationForm[kDifficultyOffset - (kLocationFormOffset + sizeof(TESForm*))];
    // SE 0xB00, VR 0x11F4. The game passes this field as the first argument to
    // GetDifficultyMultiplier (id 26503), which names it outright: SE 0x140666DBE and 0x140676851
    // both do `mov ecx,[player+0xB00]` 6 bytes before the call, and VR 0x1405ECED1 does
    // `mov ecx,[player+0x11F4]`. A live dump of the object agreed, holding 5 at +0x11F4.
    //
    // This one is not a harmless bad read. PlayerService writes it on connect, and at the SE offset
    // it landed on 0x10E8, which on VR is the element count of a player array whose data pointer
    // sits at 0x10D8. Writing a difficulty of 1 to 5 there left the count non-zero with the array
    // still null, so the game indexed null and crashed inside the HUD menu.
    int32_t difficulty;
#if TP_SKYRIMVR
    uint8_t padPostDifficulty[0x1208 - 0x11F8];
#else
    uint8_t padAFC[0xB10 - 0xAFC];
#endif
    // SE 0xB18, VR 0x1208. ApplyMasksToRenderTargets (id 27040) takes the array as its argument and
    // is called from three places in each build; the two sets pair one for one, in order, at
    // identical distances from the call, and the pair SE 0x14074A3B0 / VR 0x1406D86D0 loads it with
    // byte-identical instructions bar the displacement: `lea rcx,[rdi+0xB18]` against
    // `lea rcx,[rdi+0x1208]`, both 9 bytes before the call. Confirmed at runtime too: the object
    // holds a valid pointer there with capacity 0x40 and length 0x22, and overlayTints is null.
    GameArray<TintMask*> baseTints;
    GameArray<TintMask*>* overlayTints;

    uint8_t padPlayerEnd[0xBE0 - 0xB30];
};

static_assert(offsetof(PlayerCharacter, objectives) == kObjectivesOffset);
static_assert(offsetof(PlayerCharacter, pSkills) == kSkillsOffset);
static_assert(offsetof(PlayerCharacter, locationForm) == kLocationFormOffset);
static_assert(offsetof(PlayerCharacter, difficulty) == kDifficultyOffset);
#if TP_SKYRIMVR
static_assert(offsetof(PlayerCharacter, baseTints) == 0x1208);
static_assert(offsetof(PlayerCharacter, overlayTints) == 0x1220);
static_assert(sizeof(PlayerCharacter) == 0x12D8);
#else
static_assert(offsetof(PlayerCharacter, baseTints) == 0xB18);
static_assert(offsetof(PlayerCharacter, overlayTints) == 0xB30);
static_assert(sizeof(PlayerCharacter) == 0xBE8);
#endif
