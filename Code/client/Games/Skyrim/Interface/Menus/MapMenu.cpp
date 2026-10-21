#include <Interface/Menus/MapMenu.h>

#if TP_SKYRIMVR
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Memory.h>

// Online the map is opened from TweenMenu, which UI.cpp's kAllowList unpauses, so the frame after the
// map opens still runs the game's unpaused updates. Offline that frame is already paused. Two things go
// wrong in that frame and the map's own pause then freezes them:
// - Main's sky update job (0x1405BBF30, tail-calls Sky::Update) writes the cell's fog back over the map
//   fog MapMenu applied on its first AdvanceMovie, so the world map renders in the interior's fog.
// - The map's 3D content (MapMenu+0x48, BSFadeNode "skyVR_map.nif", fading out between 500 and 1000
//   units) is faded to 0. The floating UI panels, interior local map included, then draw fully
//   transparent while still taking input. The model is cached across opens, so it stays that way.
// Skip the sky job while the map is open (the map drives Sky::Update itself) and keep the model faded in.
// Online VR also unpauses MapMenu itself (kAllowList), which makes both of these apply every frame.
constexpr uintptr_t kMainUpdateSkyJob = 0x5BBF30;
constexpr uintptr_t kMapProcessMessage = 0x916D10; // MapMenu vtable slot 4
constexpr size_t kMenuModel = 0x48;
constexpr size_t kFadeNodeCurrentFade = 0x158;
constexpr size_t kAvObjectFlags = 0x10C;
constexpr uint32_t kIgnoreFade = 0x8000;

TP_THIS_FUNCTION(TMapProcessMessage, uint32_t, IMenu, UIMessage&);
TP_THIS_FUNCTION(TMainUpdateSky, void, void);
static TMapProcessMessage* RealMapProcessMessage;
static TMainUpdateSky* RealMainUpdateSky;
// Written by the game thread that processes UI messages and read from the client's update, which runs on the VM
// worker pool. Atomic for that second reader, not because anything here needs ordering.
static std::atomic<bool> s_mapOpen{false};
static std::atomic<bool> s_closeRequested{false};

/**
 * @brief The UIMessageQueue singleton, which is not in the address table.
 *
 * Found by call site rather than guessed. UIMessageQueue::AddMessage (id 13631) has 183 direct callers in
 * SkyrimVR 1.4.15, and all 148 of them that load `this` in the instruction immediately before the call load it
 * from this one global. Zero disagreement out of 148 is not a coincidence.
 */
constexpr uintptr_t kUIMessageQueue = 0x1F850F8;

using TAddMessage = void(void*, const BSFixedString*, uint32_t, void*);

/**
 * @brief Asks for the map to be shut. Records the wish; the game thread carries it out.
 *
 * Callable from anywhere, and that is the whole point of it doing nothing else. The client's update runs on the
 * VM worker pool, and posting the hide from there deadlocked three ways at once: the message queue's lock, the
 * string table lock that constructing a BSFixedString takes, and the magic static guard on the address lookup.
 */
void CloseMapMenu() noexcept
{
    if (s_mapOpen)
        s_closeRequested.store(true, std::memory_order_relaxed);
}

/**
 * @brief Posts a requested hide, from the player update. See the call site for why that phase and not another.
 *
 * Exchanged rather than read, so one request posts one hide however many workers asked at once.
 */
static void RunDeferredMapClose() noexcept
{
    if (!s_mapOpen || !s_closeRequested.exchange(false, std::memory_order_relaxed))
        return;

    // Through the message queue rather than by reaching into the menu, because that is the only path that
    // leaves the UI's own bookkeeping straight, and it is the one the player's own cancel ends in too.
    void* pQueue = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kUIMessageQueue);
    if (!pQueue)
        return;

    POINTER_SKYRIMSE(TAddMessage, s_addMessage, 13631);

    // Built on first use, not at static init: the game's string table does not exist that early. Same reason as
    // UI.cpp's allow list.
    static const BSFixedString s_mapName("MapMenu");

    // Send the cancel the player sends, not a bare hide. A close is kUserEvent (7) then kHide (3); ours was
    // only the kHide, which frees the 3D in one go on a UI worker while the main thread walks render
    // batches. The user event lets MapMenu shut its own world down and post the hide itself.
    //
    // BSUIMessageData taken from the call site at 0x1405D0BF5, which builds one for this message: 0x28 bytes,
    // vtable at the RVA below, the user event BSFixedString at +0x18, rest zero. Game heap, since it frees it.
    constexpr uintptr_t kMessageDataVtable = 0x16C8B38;
    constexpr size_t kMessageDataSize = 0x28;
    constexpr size_t kUserEventOffset = 0x18;

    auto* pData = static_cast<uint8_t*>(Memory::Allocate(kMessageDataSize));
    if (!pData)
        return;

    std::memset(pData, 0, kMessageDataSize);
    *reinterpret_cast<uintptr_t*>(pData) = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kMessageDataVtable;

    // The game's setter, so the string is interned and refcounted; a raw pointer copy leaves the count short.
    reinterpret_cast<BSFixedString*>(pData + kUserEventOffset)->Set("Cancel");

    s_addMessage.Get()(pQueue, &s_mapName, UIMessage::kUserEvent, pData);
}

static uint32_t HookMapProcessMessage(IMenu* apThis, UIMessage& aMessage)
{
    const bool cHiding = aMessage.eType == UIMessage::kHide || aMessage.eType == UIMessage::kForceHide;

    if (aMessage.eType == UIMessage::kShow)
        s_mapOpen = true;
    else if (cHiding)
        s_mapOpen = false;

    // A request that arrived as the map was going down must not shut the next one the instant it opens.
    if (aMessage.eType == UIMessage::kShow || cHiding)
        s_closeRequested.store(false, std::memory_order_relaxed);

    const uint32_t result = TiltedPhoques::ThisCall(RealMapProcessMessage, apThis, aMessage);

    // Set on the way in, cleared on the way out. It used to be OR'd in and never cleared, so from the first
    // open the model stayed exempt from the fade that is what removes it, leaving hidden 3D in the render path.
    if (auto* pModel = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(apThis) + kMenuModel))
    {
        if (s_mapOpen)
        {
            *reinterpret_cast<float*>(pModel + kFadeNodeCurrentFade) = 1.f;
            *reinterpret_cast<uint32_t*>(pModel + kAvObjectFlags) |= kIgnoreFade;
        }
        else if (cHiding)
        {
            *reinterpret_cast<uint32_t*>(pModel + kAvObjectFlags) &= ~kIgnoreFade;
        }
    }

    return result;
}

static void HookMainUpdateSky(void* apMain)
{
    if (s_mapOpen)
        return;

    TiltedPhoques::ThisCall(RealMainUpdateSky, apMain);
}

// The paused player update (0x1406ACF50) calls 0x1406AD080 and then the map's laser update
// (0x1406B8980) while MapMenu is open. The unpaused player update (0x1406AB7A0) calls 0x1406AD080 every
// frame but skips all pointer drawing while the UI quad is hidden, which the map does, so with the map
// unpaused the laser froze in place. Follow 0x1406AD080 with the laser update when the map is open and
// the game is not paused; paused, the game makes that call itself.
constexpr uintptr_t kPlayerMapModeUpdate = 0x6AD080;
constexpr uintptr_t kMapPointerUpdate = 0x6B8980;
constexpr uintptr_t kMainPausedByte = 0x2FEB76B;

TP_THIS_FUNCTION(TPlayerMapModeUpdate, void, void, float, float);
TP_THIS_FUNCTION(TMapPointerUpdate, void, void, float);
static TPlayerMapModeUpdate* RealPlayerMapModeUpdate;
static TMapPointerUpdate* MapPointerUpdate;

static void HookPlayerMapModeUpdate(void* apPlayer, float aDelta, float aRealDelta)
{
    TiltedPhoques::ThisCall(RealPlayerMapModeUpdate, apPlayer, aDelta, aRealDelta);

    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (s_mapOpen && *reinterpret_cast<const uint8_t*>(exe + kMainPausedByte) == 0)
        TiltedPhoques::ThisCall(MapPointerUpdate, apPlayer, aDelta);

    // From the player update, inside Main::Loop, which is the phase the player's own cancel is posted from.
    // HookMainLoop runs before Main::Loop has done anything and took the menu down mid-frame.
    RunDeferredMapClose();
}

// Shader property controllers write a double buffer that the frame parity (0x1434231A0) selects. The
// main loop flips the parity once per unpaused frame. MapMenu's per-frame update (0x14091BE70, from
// AdvanceMovie) flips it itself after updating the map world, because while paused nobody else does.
// With the map unpaused both flip, and the map's animated fog and clouds blink. Undo the map's flip when
// the game is not paused.
constexpr uintptr_t kMapWorldUpdate = 0x91BE70;
constexpr uintptr_t kFrameParity = 0x34231A0;

using TMapWorldUpdate = void(float);
static TMapWorldUpdate* RealMapWorldUpdate;

static void HookMapWorldUpdate(float aDelta)
{
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* pParity = reinterpret_cast<uint32_t*>(exe + kFrameParity);
    const uint32_t parityBefore = *pParity;

    RealMapWorldUpdate(aDelta);

    // The flip is conditional inside the map update, so only revert one that happened.
    if (*pParity != parityBefore && *reinterpret_cast<const uint8_t*>(exe + kMainPausedByte) == 0)
        *pParity = parityBefore;
}
#endif

static TiltedPhoques::Initializer s_init(
    []() {
// Disabled because the mapmenu in first person breaks.
// I fix that later, but for now it doesn't break gameplay.
#if 0
    // https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/Menus/MapMenuEx.cpp
    // Of course this isnt perfect yet. but we"ll see

    VersionDbPtr<void*> hookLoc(53112);
    TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x53, 4);
    TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x9D, 2);
    TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x9F, 1);
#endif

#if TP_SKYRIMVR
        const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        RealMapProcessMessage = reinterpret_cast<TMapProcessMessage*>(exe + kMapProcessMessage);
        TP_HOOK(&RealMapProcessMessage, HookMapProcessMessage);
        RealMainUpdateSky = reinterpret_cast<TMainUpdateSky*>(exe + kMainUpdateSkyJob);
        TP_HOOK(&RealMainUpdateSky, HookMainUpdateSky);
        MapPointerUpdate = reinterpret_cast<TMapPointerUpdate*>(exe + kMapPointerUpdate);
        RealPlayerMapModeUpdate = reinterpret_cast<TPlayerMapModeUpdate*>(exe + kPlayerMapModeUpdate);
        TP_HOOK(&RealPlayerMapModeUpdate, HookPlayerMapModeUpdate);
        RealMapWorldUpdate = reinterpret_cast<TMapWorldUpdate*>(exe + kMapWorldUpdate);
        TP_HOOK(&RealMapWorldUpdate, HookMapWorldUpdate);
#endif
    });
