
#include <BSGraphics/BSGraphicsRenderer.h>

#if TP_SKYRIMVR
#include <Systems/VRMenuOverlay.h>
#endif

struct BSInputDeviceManager;

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    if (!BSGraphics::GetMainWindow()->IsForeground())
        return;

#if TP_SKYRIMVR
    /**
     * The same trick as the line above, for the same reason: the cheapest way to stop input reaching the game
     * is not to let it look.
     *
     * The menu drawn into the frame reads the controllers itself, and nothing takes them away from the game
     * while it does, so a trigger that clicks a button would also swing a sword and a stick that moves a
     * cursor would also turn the player. Withholding is only ever asked for once both hands are idle, so what
     * the game is left holding is a clear hand rather than whatever was pressed at the time.
     */
    if (VRMenuOverlay::ShouldWithholdGameInput())
        return;
#endif

    BSInputDeviceManager_PollInputDevices(inputDeviceMgr, afDelta);
}

static TiltedPhoques::Initializer s_initInputDeviceManager(
    []()
    {
        const VersionDbPtr<void> pollInputDevices(68617);

        BSInputDeviceManager_PollInputDevices = static_cast<decltype(BSInputDeviceManager_PollInputDevices)>(pollInputDevices.GetPtr());

        TP_HOOK_IMMEDIATE(&BSInputDeviceManager_PollInputDevices, &Hook_BSInputDeviceManager_PollInputDevices);
    });
