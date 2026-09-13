
// The game calls it statsmenu.
#include <Interface/Menus/SkillsMenu.h>

// Where the patched instruction sits inside its function. SkyrimVR 1.4.15 and Anniversary Edition
// were compiled from different revisions, so an offset is only valid for the build it was read
// from. Tools/vr_addresses/patches.mjs checks both columns against the two exes.
#if TP_SKYRIMVR
constexpr size_t kMainThreadCheck = 0x91C;
constexpr size_t kFreezeFrameFlag = 0xBB6;
constexpr size_t kMenuUpdateCheck = 0x109C;
constexpr size_t kCanProcessGuard = 0x98;
#else
constexpr size_t kMainThreadCheck = 0x84E;
constexpr size_t kFreezeFrameFlag = 0xA10;
constexpr size_t kMenuUpdateCheck = 0x1040;
constexpr size_t kCanProcessGuard = 0x46;
#endif

static TiltedPhoques::Initializer s_skillsMenuInit(
    []()
    {
        // https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/Menus/StatsMenuEx.cpp
        // Hoooks from souls RE
        // Fix for menu not appearing
        VersionDbPtr<uint8_t> ProcessMessage(52510);
        TiltedPhoques::Nop(ProcessMessage.Get() + kMainThreadCheck, 6);
        // Prevent setting kFreezeFrameBackground flag
        TiltedPhoques::Nop(ProcessMessage.Get() + kFreezeFrameFlag, 4);
        // Keep the menu updated
        TiltedPhoques::Nop(ProcessMessage.Get() + kMenuUpdateCheck, 2);

        // Fix for controls not working. Both Nops together cover one 6 byte jnz.
        VersionDbPtr<uint8_t> controlPatch(52518);
        TiltedPhoques::Nop(controlPatch.Get() + kCanProcessGuard, 4);
        TiltedPhoques::Nop(controlPatch.Get() + kCanProcessGuard + 4, 2);
    });
