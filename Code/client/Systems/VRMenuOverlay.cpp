#include <TiltedOnlinePCH.h>

#include <Systems/VRMenuOverlay.h>

#if TP_SKYRIMVR

#include <ModCompat/openvr.h>

#include <OverlayApp.hpp>
#include <OverlayRenderHandler.hpp>

#include <Systems/VREyeOverlay.h>

#include <Services/InputService.h>
#include <Services/OverlayService.h>
#include <World.h>

#include <d3d11.h>
#include <wrl.h>

namespace
{
/**
 * @brief What the page is rendered at, independent of the game's resolution.
 *
 * The desktop handler sizes itself from the swapchain, which in VR is the mirror window and has nothing to do
 * with what the headset shows. A fixed size keeps the quad's aspect ratio and sharpness under our control.
 */
constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;

/**
 * @brief Where the quad goes, in metres, read once.
 *
 * Sizing is not free here, because the page's own layout decides where things end up on the quad. The controls
 * panel is anchored to the bottom of the page by root.component.scss, so whatever it is anchored to sits half
 * the quad's height below eye level: at 6.4m wide that is 56 degrees down, which is unreadable, and at 2m it
 * is 17. Making the quad bigger to read it better therefore does the opposite. Modals centre themselves and
 * behave the other way round, so no one size suits both until the page stops putting content at its edges.
 *
 * The whole page fits inside this field of view at 2m, which is the property worth keeping: nothing on it
 * needs a head turn to find. At this distance 0.1m of shift is roughly five degrees.
 *
 * Every field can be overridden by an environment variable, because tuning this by feel takes a headset on a
 * head and a rebuild between each guess is a poor way to spend that.
 */
struct Placement
{
    float widthInMetres = 2.0f;
    float distanceInMetres = 1.2f;

    // Positive is right. Left at nothing: the panel centres itself horizontally as of the VR change to the page.
    float rightShiftInMetres = 0.0f;

    // Positive is up, lifting the bottom anchored panel nearer to eye level without shrinking the quad further.
    float upShiftInMetres = 0.0f;
};

const Placement& GetPlacement() noexcept
{
    static const Placement s_placement = []
    {
        Placement placement;

        auto read = [](const char* acpName, float& aValue, const float acMin, const float acMax)
        {
            char buffer[32]{};

            if (GetEnvironmentVariableA(acpName, buffer, sizeof(buffer)) == 0)
                return;

            char* pEnd = nullptr;
            const float cParsed = std::strtof(buffer, &pEnd);

            if (pEnd == buffer || !std::isfinite(cParsed) || cParsed < acMin || cParsed > acMax)
            {
                spdlog::error("VR menu: {} reads as '{}', which is not a length between {} and {} metres, so it is ignored", acpName, buffer, acMin, acMax);
                return;
            }

            spdlog::info("VR menu: {} puts that at {}m instead of {}m", acpName, cParsed, aValue);
            aValue = cParsed;
        };

        read("SKYRIM_TOGETHER_VR_MENU_WIDTH", placement.widthInMetres, 0.1f, 50.f);
        read("SKYRIM_TOGETHER_VR_MENU_DISTANCE", placement.distanceInMetres, 0.2f, 50.f);
        read("SKYRIM_TOGETHER_VR_MENU_RIGHT", placement.rightShiftInMetres, -50.f, 50.f);
        read("SKYRIM_TOGETHER_VR_MENU_UP", placement.upShiftInMetres, -50.f, 50.f);

        return placement;
    }();

    return s_placement;
}

/**
 * @brief The interface the controllers are read through, or null, which decides how the menu is driven.
 *
 * Asked for by version, unlike everything VREyeOverlay borrows from the game, and that is the whole reason
 * there are two ways of driving the menu. SteamVR answers every version ever shipped. A runtime that only
 * answers what the game asks for leaves this null, and then the menu is a mouse affair.
 */
vr::IVRSystem* s_pVRSystem = nullptr;

// The three of them, tracked so each acts on the press rather than every frame it is held.
bool s_openWasHeld = false;
bool s_closeWasHeld = false;
bool s_triggerWasHeld = false;

/**
 * @brief The cursor: where on the page it is, whether it is live, and which input last moved it.
 *
 * Unlike a laser, a cursor persists. Aiming sets it, the stick nudges it from wherever aiming left it, and
 * dropping the arm leaves it alone rather than dragging it to the floor.
 */
bool s_cursorOnPage = false;
float s_cursorAcross = 0.5f;
float s_cursorDown = 0.5f;
bool s_stickHasCursor = false;
float s_rayWhenStickTook[2]{};
std::chrono::steady_clock::time_point s_cursorMovedAt{};

// How far the stick has to go before it counts, and how fast it then moves the cursor, in page widths a second.
constexpr float kStickDeadzone = 0.2f;
constexpr float kCursorPageWidthsPerSecond = 0.9f;

// How far the aim has to travel, as a fraction of the page, to take the cursor back off the stick.
constexpr float kReaimFraction = 0.12f;

/**
 * @brief Whether the stick drives a cursor, and with it whether the game is kept away from the controllers.
 *
 * One switch for both because they are one feature: a stick that moves a cursor is also a stick that turns the
 * player, and a trigger that clicks is also a trigger that swings a sword. Turning this off restores exactly
 * what came before it, aiming only, with the game seeing every press as well.
 */
bool UseStickCursor() noexcept
{
    static const bool s_use = []
    {
        char buffer[32]{};

        if (GetEnvironmentVariableA("SKYRIM_TOGETHER_VR_MENU_STICK", buffer, sizeof(buffer)) == 0)
            return true;

        const bool cUse = buffer[0] != '0';

        spdlog::info("VR menu: SKYRIM_TOGETHER_VR_MENU_STICK turns the stick cursor {}", cUse ? "on" : "off");

        return cUse;
    }();

    return s_use;
}

/**
 * @brief Whether the game is currently being kept from polling its input devices.
 *
 * Read from the game's own thread in the poll hook, written here on the render thread.
 *
 * Nothing captures the controllers on this path, so every press the menu reads reaches the game as well. What
 * stops that is simply not letting the game look. The catch is that not looking freezes whatever it last saw,
 * so starting while a button is down would leave it down for good, and the chord that opens the menu is a
 * button being down. Hence the wait for a clear hand below before the freeze begins.
 */
std::atomic<bool> s_withholdGameInput{false};

/**
 * @brief When the menu went up, and how long the closing grip is ignored for afterwards.
 *
 * The chord that opens the menu holds a grip, and the moment the overlay appears SteamVR routes that same
 * still-held grip to it as a middle click, which closed the menu about fifteen milliseconds after it opened
 * and then let the chord open it again, over and over. Measured on 2026-08-17: open at 01:39:11.949, closed
 * at 01:39:11.978, seven times in under a second.
 *
 * So the grip that opened it has to be spent before another one can close it. A short deadline does that
 * without needing to track which physical button is still down.
 */
std::chrono::steady_clock::time_point s_shownAt{};
constexpr double kCloseGraceSeconds = 0.7;

// The page itself could not be created, which nothing can survive.
bool s_pageBroken = false;
bool s_visible = false;

Microsoft::WRL::ComPtr<ID3D11Texture2D> s_pTexture;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_pTextureView;
ID3D11Device* s_pDevice = nullptr;
ID3D11DeviceContext* s_pContext = nullptr;

// CEF paints on its own thread and D3D11 uploads happen on the render thread, so the page lands here first.
std::vector<uint8_t> s_pixels;
std::mutex s_pixelLock;
bool s_dirty = false;

// Defined below, declared here because the input pump closes the menu and sits above it.
void ApplyVisible(bool aVisible) noexcept;

/**
 * @brief Finds the interface the controllers are read through, once, and says how the menu will be driven.
 *
 * A runtime that serves only what the game asks for gets no controllers here, and the menu falls to the mouse.
 * That is not the same as the menu failing: what draws it borrows the game's own interfaces and is unaffected.
 *
 * Setting SKYRIM_TOGETHER_VR_MOUSE_MODE reproduces that install on a machine that has SteamVR, which is the
 * only way to try the mouse on any machine this can be developed on.
 */
void AcquireSystem() noexcept
{
    static bool s_tried = false;

    if (s_tried)
        return;

    s_tried = true;

    if (GetEnvironmentVariableA("SKYRIM_TOGETHER_VR_MOUSE_MODE", nullptr, 0) != 0)
    {
        spdlog::info("VR menu: SKYRIM_TOGETHER_VR_MOUSE_MODE is set, so the controllers are left alone and the mouse drives the menu");
        return;
    }

    // The game brings openvr_api.dll in and initialises it long before this runs, so this only borrows what is
    // already there. Resolved by hand rather than linked, because an import would have to resolve at process
    // start, when the dll is not yet anywhere the loader will look.
    const HMODULE cApi = GetModuleHandleA("openvr_api.dll");

    if (!cApi)
    {
        spdlog::error("VR menu: openvr_api.dll is not loaded, so the mouse drives the menu");
        return;
    }

    using TGetGenericInterface = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
    const auto pGetInterface = reinterpret_cast<TGetGenericInterface>(GetProcAddress(cApi, "VR_GetGenericInterface"));

    if (!pGetInterface)
    {
        spdlog::error("VR menu: openvr_api.dll has no VR_GetGenericInterface, so the mouse drives the menu");
        return;
    }

    vr::EVRInitError error = vr::VRInitError_None;
    s_pVRSystem = static_cast<vr::IVRSystem*>(pGetInterface(vr::IVRSystem_Version, &error));

    if (!s_pVRSystem)
    {
        spdlog::info("VR menu: this runtime does not serve {} (error {}), so the mouse drives the menu", vr::IVRSystem_Version, static_cast<int>(error));
        return;
    }

    spdlog::info("VR menu: {} answered, so the controllers point at the menu and their sticks move its cursor", vr::IVRSystem_Version);
}

bool EnsureTexture() noexcept
{
    if (s_pTexture)
        return true;

    if (!s_pDevice)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // CEF paints BGRA, so the texture takes it without a conversion pass.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(s_pDevice->CreateTexture2D(&desc, nullptr, s_pTexture.GetAddressOf())))
    {
        spdlog::error("VR menu: could not create the {}x{} overlay texture", kWidth, kHeight);
        s_pageBroken = true;
        return false;
    }

    // For the path that draws the page itself rather than handing it to SteamVR. SteamVR wants the texture,
    // a shader wants a view of it, and the two cost nothing to keep side by side.
    if (FAILED(s_pDevice->CreateShaderResourceView(s_pTexture.Get(), nullptr, s_pTextureView.ReleaseAndGetAddressOf())))
        spdlog::error("VR menu: no shader resource view over the page, so it cannot be drawn into the frame");

    return true;
}

// shows another.
glm::vec3 Translation(const vr::HmdMatrix34_t& acPose) noexcept
{
    return {acPose.m[0][3], acPose.m[1][3], acPose.m[2][3]};
}

// glm takes its columns first, and a pose's rotation is stored a row at a time, so the indices swap. Same
// swap as ReadXformAt in HandPoseService, for the same reason.
glm::mat3 Rotation(const vr::HmdMatrix34_t& acPose) noexcept
{
    return {acPose.m[0][0], acPose.m[1][0], acPose.m[2][0], acPose.m[0][1], acPose.m[1][1],
            acPose.m[2][1], acPose.m[0][2], acPose.m[1][2], acPose.m[2][2]};
}

/**
 * @brief Which of a controller's five axes is its stick.
 *
 * Not something to assume. Legacy input puts a stick on whichever axis the runtime's binding for that
 * controller says, and the trigger and grip are axes too, so guessing at index nought and finding a trigger
 * there would read a cursor speed off how hard the trigger is squeezed. The runtime will simply say: each axis
 * declares its own type. A trackpad is taken if there is no stick, since a Vive wand has nothing else.
 */
uint32_t FindStickAxis(const vr::TrackedDeviceIndex_t acHand) noexcept
{
    // Per device, because both hands ask and a single slot would have them evicting each other's answer every
    // frame, asking the runtime again each time and saying so in the log each time.
    static int8_t s_axisPerDevice[vr::k_unMaxTrackedDeviceCount]{};
    static bool s_asked[vr::k_unMaxTrackedDeviceCount]{};

    if (acHand >= vr::k_unMaxTrackedDeviceCount)
        return 0;

    if (s_asked[acHand])
        return static_cast<uint32_t>(s_axisPerDevice[acHand]);

    s_asked[acHand] = true;
    s_axisPerDevice[acHand] = 0;

    int32_t fallback = -1;

    for (uint32_t axis = 0; axis < vr::k_unControllerStateAxisCount; ++axis)
    {
        const auto cType = static_cast<vr::EVRControllerAxisType>(
            s_pVRSystem->GetInt32TrackedDeviceProperty(acHand, static_cast<vr::ETrackedDeviceProperty>(vr::Prop_Axis0Type_Int32 + axis)));

        if (cType == vr::k_eControllerAxis_Joystick)
        {
            s_axisPerDevice[acHand] = static_cast<int8_t>(axis);
            spdlog::info("VR menu: device {} calls axis {} a joystick, so that is what moves the cursor", acHand, axis);

            return axis;
        }

        if (cType == vr::k_eControllerAxis_TrackPad && fallback < 0)
            fallback = static_cast<int32_t>(axis);
    }

    s_axisPerDevice[acHand] = fallback >= 0 ? static_cast<int8_t>(fallback) : 0;

    spdlog::info("VR menu: device {} declares no joystick, so the cursor takes axis {}", acHand, s_axisPerDevice[acHand]);

    return static_cast<uint32_t>(s_axisPerDevice[acHand]);
}

/**
 * @brief The stick, from whichever hand is pushing one.
 *
 * Not just the hand that points. In this game the left stick is the walking stick, so it is the one a hand goes
 * to without thinking about it, and deciding for the player which of the two counts is a way to have them
 * conclude the feature does not work.
 */
glm::vec2 ReadStick() noexcept
{
    glm::vec2 stick(0.f);

    for (const vr::ETrackedControllerRole cRole : {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand})
    {
        const vr::TrackedDeviceIndex_t cHand = s_pVRSystem->GetTrackedDeviceIndexForControllerRole(cRole);

        if (cHand == vr::k_unTrackedDeviceIndexInvalid)
            continue;

        vr::VRControllerState_t state{};

        if (!s_pVRSystem->GetControllerState(cHand, &state, sizeof(state)))
            continue;

        const vr::VRControllerAxis_t& cAxis = state.rAxis[FindStickAxis(cHand)];
        const glm::vec2 cCandidate(cAxis.x, cAxis.y);

        if (glm::length(cCandidate) > glm::length(stick))
            stick = cCandidate;
    }

    return stick;
}

/**
 * @brief Points at the menu with the right controller and clicks it, for the path with no compositor to do it.
 *
 * The SteamVR path gets all of this free: the compositor draws the laser, works out where it lands and sends
 * mouse events in page pixels. Drawing the page ourselves means doing that ourselves too.
 *
 * Everything here is in the headset's own space, taken from the poses the runtime reports rather than from the
 * game's nodes. The quad is defined in that space, so no conversion and no guessing at whose axes point where
 * is needed: a ray goes in, a point on a rectangle comes out. The controller's forward is its own -z, which is
 * the one direction OpenVR fixes for every device there is.
 *
 * This needs the interface version we ask for by name, so on an install that serves the game's version and
 * nothing else there is no controller here and the menu is a mouse and keyboard affair. Worth revisiting with
 * IVRCompositor::GetLastPoses, which is in the part of that vtable every version agrees on.
 */
void PointAndClick() noexcept
{
    if (!s_pVRSystem || !s_visible)
        return;

    const vr::TrackedDeviceIndex_t cHand = s_pVRSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);

    if (cHand == vr::k_unTrackedDeviceIndexInvalid)
        return;

    // Before any of the geometry, because the geometry is allowed to come to nothing. An arm hanging by a side
    // points at the floor, and that is exactly when the stick is what the hand is reaching for.
    vr::VRControllerState_t state{};

    if (!s_pVRSystem->GetControllerState(cHand, &state, sizeof(state)))
        return;

    const Placement& cPlacement = GetPlacement();

    float cRayAcross = 0.f;
    float cRayDown = 0.f;
    bool cRayOnPage = false;

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    s_pVRSystem->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, poses, vr::k_unMaxTrackedDeviceCount);

    const vr::TrackedDevicePose_t& cHead = poses[vr::k_unTrackedDeviceIndex_Hmd];
    const vr::TrackedDevicePose_t& cWand = poses[cHand];

    if (cHead.bPoseIsValid && cWand.bPoseIsValid)
    {
        const glm::mat3 cIntoHead = glm::transpose(Rotation(cHead.mDeviceToAbsoluteTracking));
        const glm::vec3 cHeadAt = Translation(cHead.mDeviceToAbsoluteTracking);

        const glm::mat3 cWandRotation = Rotation(cWand.mDeviceToAbsoluteTracking);
        const glm::vec3 cOrigin = cIntoHead * (Translation(cWand.mDeviceToAbsoluteTracking) - cHeadAt);
        const glm::vec3 cDirection = cIntoHead * -glm::vec3(cWandRotation[2]);

        // The quad is in front, at negative z, and so must the ray be going. Otherwise it meets the plane
        // behind the player's head, where the arithmetic still works and the answer is nonsense.
        const float cAlong = cDirection.z < -0.001f ? (-cPlacement.distanceInMetres - cOrigin.z) / cDirection.z : -1.f;

        if (cAlong > 0.f)
        {
            const glm::vec3 cHit = cOrigin + cDirection * cAlong;

            const float cQuadWidth = cPlacement.widthInMetres;
            const float cQuadHeight = cQuadWidth * static_cast<float>(kHeight) / static_cast<float>(kWidth);

            // Across and down the page, so the vertical one counts from the top edge while the space counts up.
            cRayAcross = (cHit.x - (cPlacement.rightShiftInMetres - cQuadWidth * 0.5f)) / cQuadWidth;
            cRayDown = ((cPlacement.upShiftInMetres + cQuadHeight * 0.5f) - cHit.y) / cQuadHeight;
            cRayOnPage = cRayAcross >= 0.f && cRayAcross <= 1.f && cRayDown >= 0.f && cRayDown <= 1.f;
        }
    }

    // Aiming moves the cursor while the stick is idle, and the stick moves it from wherever aiming left it.
    // Whichever was touched last keeps it, so an arm can be dropped without the cursor going with it.
    if (UseStickCursor())
    {
        const glm::vec2 cStick = ReadStick();
        const float cStickX = cStick.x;
        const float cStickY = cStick.y;
        const float cPush = glm::length(cStick);

        if (cPush > kStickDeadzone)
        {
            const auto cNow = std::chrono::steady_clock::now();
            const float cElapsed = std::chrono::duration<float>(cNow - s_cursorMovedAt).count();

            s_cursorMovedAt = cNow;

            // A frame's worth at most, so a loading screen or a breakpoint does not fling the cursor away.
            const float cStep = kCursorPageWidthsPerSecond * std::min(cElapsed, 0.05f);

            // The page is wider than it is tall, so the same push has to cover proportionally more of the
            // height to move the cursor at one speed rather than at two.
            s_cursorAcross = std::clamp(s_cursorAcross + cStickX * cStep, 0.f, 1.f);
            s_cursorDown = std::clamp(s_cursorDown - cStickY * cStep * static_cast<float>(kWidth) / static_cast<float>(kHeight), 0.f, 1.f);

            s_stickHasCursor = true;

            if (cRayOnPage)
            {
                s_rayWhenStickTook[0] = cRayAcross;
                s_rayWhenStickTook[1] = cRayDown;
            }
        }
        else if (s_stickHasCursor && cRayOnPage)
        {
            // Handing it back on deliberate aiming rather than on any movement at all, because a held arm
            // never stops moving and a threshold small enough to feel responsive is smaller than a tremor.
            const float cMoved = std::hypot(cRayAcross - s_rayWhenStickTook[0], cRayDown - s_rayWhenStickTook[1]);

            if (cMoved > kReaimFraction)
                s_stickHasCursor = false;
        }
    }
    else
    {
        s_stickHasCursor = false;
    }

    if (!s_stickHasCursor && cRayOnPage)
    {
        s_cursorAcross = cRayAcross;
        s_cursorDown = cRayDown;
    }

    // Live when something is driving it. Aiming off the page with no stick in play leaves the cursor where it
    // was and stops showing it, which is what the compositor's laser does when it misses.
    s_cursorOnPage = s_stickHasCursor || cRayOnPage;

    OverlayApp* pApp = World::Get().GetOverlayService().GetOverlayApp();

    if (!pApp)
        return;

    const auto cX = static_cast<uint16_t>(s_cursorAcross * kWidth);
    const auto cY = static_cast<uint16_t>(s_cursorDown * kHeight);

    if (s_cursorOnPage)
        pApp->InjectMouseMove(cX, cY, 0);

    // Pressing counts only while the cursor is live. Releasing counts wherever it happens, or letting go after
    // aiming away would never be reported and the page would be left holding a button down for good.
    // Not constexpr: openvr's ButtonMaskFromId is a plain inline.
    const bool cTriggerHeld =
        (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0 && (s_cursorOnPage || s_triggerWasHeld);

    if (cTriggerHeld != s_triggerWasHeld)
        pApp->InjectMouseButton(cX, cY, MBT_LEFT, !cTriggerHeld, 0);

    s_triggerWasHeld = cTriggerHeld;
}

/**
 * @brief A grip closes the menu.
 *
 * Either hand does it, as long as the grip that opened the menu has been let go of first. Without that the
 * opening chord, which holds a grip, closes the menu on the very next frame.
 */
void PollEyeMenuClose() noexcept
{
    if (!s_pVRSystem || !s_visible)
        return;

    uint64_t pressed = 0;
    bool sticksCentred = true;

    for (const vr::ETrackedControllerRole cRole : {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand})
    {
        const vr::TrackedDeviceIndex_t cHand = s_pVRSystem->GetTrackedDeviceIndexForControllerRole(cRole);
        vr::VRControllerState_t state{};

        if (cHand != vr::k_unTrackedDeviceIndexInvalid && s_pVRSystem->GetControllerState(cHand, &state, sizeof(state)))
        {
            pressed |= state.ulButtonPressed;

            const vr::VRControllerAxis_t& cStick = state.rAxis[FindStickAxis(cHand)];

            if (std::hypot(cStick.x, cStick.y) > kStickDeadzone)
                sticksCentred = false;
        }
    }

    /**
     * The freeze starts once both hands are idle and not before, because freezing captures whatever the game
     * last saw. Beginning while the opening chord is still held would leave the game believing that grip is
     * held for the rest of the session, and a stick left off centre would have the player turning for ever.
     *
     * A stick counts as movement here as much as a button does, for exactly that reason.
     */
    if (UseStickCursor() && !s_withholdGameInput.load() && pressed == 0 && sticksCentred)
    {
        s_withholdGameInput.store(true);
        spdlog::info("VR menu: the hands are clear, so the game stops seeing the controllers until the menu closes");
    }

    // A grip on its own. With the menu button too it is the chord that opens the menu, and that must not also
    // be what closes it.
    const bool cCloseHeld = (pressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0 &&
                            (pressed & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) == 0;

    if (cCloseHeld && !s_closeWasHeld && std::chrono::duration<double>(std::chrono::steady_clock::now() - s_shownAt).count() >= kCloseGraceSeconds)
    {
        spdlog::info("VR menu: closing the menu");

        ApplyVisible(false);

        World::Get().GetRunner().Queue([]() { InputService::SetUI(false); });
    }

    s_closeWasHeld = cCloseHeld;
}

void ApplyVisible(bool aVisible) noexcept
{
    s_visible = aVisible;

    if (aVisible)
    {
        // From the middle, so the stick has somewhere to start from on a hand that is not pointing at anything.
        s_cursorAcross = 0.5f;
        s_cursorDown = 0.5f;
    }
    else
    {
        s_cursorOnPage = false;
        s_stickHasCursor = false;
        s_triggerWasHeld = false;

        // Whatever the game last saw was a clear hand, since that is the only state the freeze can begin from.
        s_withholdGameInput.store(false);
    }

    if (aVisible)
        s_shownAt = std::chrono::steady_clock::now();
}

/**
 * @brief B with a grip opens the menu.
 *
 * Legacy controller state rather than SteamVR's action system, because an action would mean shipping a
 * manifest and asking the player to bind it, and the game is a legacy input application so the runtime is
 * already serving that path. The masks were read off this client's own log rather than assumed.
 *
 * Nothing takes the controllers away while the menu is up, so this keeps reading and PollEyeMenuClose does
 * the closing from the same state. Under a compositor overlay it could not: that reads zero for both hands.
 */
void PollMenuChords() noexcept
{
    if (!s_pVRSystem)
        return;

    uint64_t pressed[2]{};

    for (size_t hand = 0; hand < 2; ++hand)
    {
        const vr::ETrackedControllerRole cRole = hand == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
        const vr::TrackedDeviceIndex_t cDevice = s_pVRSystem->GetTrackedDeviceIndexForControllerRole(cRole);

        vr::VRControllerState_t state{};

        if (cDevice != vr::k_unTrackedDeviceIndexInvalid && s_pVRSystem->GetControllerState(cDevice, &state, sizeof(state)))
            pressed[hand] = state.ulButtonPressed;
    }

    // B with the grip. B is the upper face button, which arrives as ApplicationMenu; the lower one, A, is
    // k_EButton_A. Not constexpr: openvr's ButtonMaskFromId is a plain inline.
    const uint64_t cOpenMask = vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu) | vr::ButtonMaskFromId(vr::k_EButton_Grip);

    // Either hand. Only opening is handled here; PollEyeMenuClose does the closing.
    const bool cOpenHeld = (pressed[0] & cOpenMask) == cOpenMask || (pressed[1] & cOpenMask) == cOpenMask;

    // Edge triggered, so holding the chord does nothing after the first frame.
    if (cOpenHeld && !s_openWasHeld && !s_visible)
    {
        spdlog::info("VR menu: opening the menu");

        ApplyVisible(true);

        // The page, the input hook and the cursor are not this thread's to touch. The quad above is, and it
        // is already up by the time this runs.
        World::Get().GetRunner().Queue([]() { InputService::SetUI(true); });
    }

    s_openWasHeld = cOpenHeld;
}

struct VRRenderHandler : TiltedPhoques::OverlayRenderHandler
{
    VRRenderHandler(ID3D11Device* apDevice, ID3D11DeviceContext* apContext) noexcept
    {
        s_pDevice = apDevice;
        s_pContext = apContext;

        s_pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    }

    void Create() override {}

    void Reset() override
    {
        std::scoped_lock lock(s_pixelLock);

        s_pTextureView.Reset();
        s_pTexture.Reset();
        s_dirty = true;
    }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& aRect) override { aRect = CefRect(0, 0, kWidth, kHeight); }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType aType, const RectList&, const void* acpBuffer, int aWidth, int aHeight) override
    {
        if (aType != PET_VIEW || !acpBuffer)
            return;

        // A resize would mean CEF ignored the rect we asked for, and copying a differently shaped page into
        // this buffer would run off the end of it.
        if (aWidth != static_cast<int>(kWidth) || aHeight != static_cast<int>(kHeight))
            return;

        std::scoped_lock lock(s_pixelLock);

        std::memcpy(s_pixels.data(), acpBuffer, s_pixels.size());
        s_dirty = true;
    }

    /**
     * @brief Uploads whatever CEF last painted and hands it to SteamVR. Called once a frame on the render
     * thread, which is the only thread allowed to touch the device context here.
     */
    void Render() override
    {
        if (s_pageBroken || !EnsureTexture())
            return;

        {
            std::scoped_lock lock(s_pixelLock);

            if (s_dirty)
            {
                s_pContext->UpdateSubresource(s_pTexture.Get(), 0, nullptr, s_pixels.data(), kWidth * 4, 0);
                s_dirty = false;
            }
        }

        AcquireSystem();

        // All three do nothing without controllers, which is what mouse mode is: the page is still drawn and
        // still placed, and InputService drives it from the desktop instead.
        PollMenuChords();
        PointAndClick();
        PollEyeMenuClose();
    }

    IMPLEMENT_REFCOUNTING(VRRenderHandler);
};
} // namespace

TiltedPhoques::OverlayRenderHandler* VRMenuOverlay::CreateRenderHandler(ID3D11Device* apDevice, ID3D11DeviceContext* apContext) noexcept
{
    return new VRRenderHandler(apDevice, apContext);
}

void VRMenuOverlay::SetVisible(bool aVisible) noexcept
{
    ApplyVisible(aVisible);
}

void VRMenuOverlay::SetMouseCursor(const float aAcross, const float aDown) noexcept
{
    // Only with no controllers to read. With them the marker follows the hand, and the window procedure
    // reports the pointer's position on every message it sees rather than only when it moves, so a still mouse
    // would be forever dragging the marker back from wherever the hand had just put it.
    if (s_pVRSystem)
        return;

    s_cursorAcross = aAcross;
    s_cursorDown = aDown;
    s_cursorOnPage = true;
}

bool VRMenuOverlay::ShouldWithholdGameInput() noexcept
{
    return s_withholdGameInput.load();
}

VRMenuOverlay::Page VRMenuOverlay::GetPage() noexcept
{
    const Placement& cPlacement = GetPlacement();

    return {s_pTextureView.Get(),
            kWidth,
            kHeight,
            s_visible,
            cPlacement.widthInMetres,
            cPlacement.distanceInMetres,
            cPlacement.rightShiftInMetres,
            cPlacement.upShiftInMetres,
            s_cursorOnPage,
            s_cursorAcross,
            s_cursorDown};
}

#endif
