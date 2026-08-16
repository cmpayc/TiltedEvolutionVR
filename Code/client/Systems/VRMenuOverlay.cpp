#include <TiltedOnlinePCH.h>

#include <Systems/VRMenuOverlay.h>

#if TP_SKYRIMVR

#include <ModCompat/openvr.h>

#include <OverlayApp.hpp>
#include <OverlayRenderHandler.hpp>

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
 * @brief Where the quad sits, in metres: how wide, how far in front of the eyes, and how far to the right.
 *
 * The page centres the connect window in its own viewport, so the sideways offset is the quad's placement
 * rather than the layout. The headset's tracked pose origin is not the point between the eyes, which is the
 * likely reason a quad centred on it does not look centred, and it is easier to correct by eye than to derive.
 *
 * At this distance 0.1m is roughly five degrees, if the shift needs another nudge either way.
 */
constexpr float kWidthInMetres = 3.2f;
constexpr float kDistanceInMetres = 1.2f;

/**
 * @brief How far right of the headset's forward axis the quad sits. Positive is right.
 *
 * Half the quad's width, because the window is not where the page's own CSS says it should be: the popup
 * container centres it in the viewport, yet it lands in the left half of the texture, so the quad has to be
 * pushed right by about half a screen to put the window in front of the player.
 *
 * That is treating the symptom. The cause is worth finding, and if the window really does render into the
 * left half of the page then the tidier fix is SetOverlayTextureBounds cropping to the part that is used,
 * which would also stop the empty half eating the field of view.
 */
constexpr float kRightShiftInMetres = 0.0f;

vr::IVROverlay* s_pVROverlay = nullptr;
vr::IVRSystem* s_pVRSystem = nullptr;
vr::VROverlayHandle_t s_overlay = vr::k_ulOverlayHandleInvalid;

// The two hotkeys, tracked so each acts on the press rather than every frame it is held.
bool s_openWasHeld = false;
bool s_closeWasHeld = false;

/**
 * @brief When the menu went up, and how long the closing grip is ignored for afterwards.
 *
 * The chord that opens the menu holds a grip, and the moment the overlay appears SteamVR routes that same
 * still-held grip to it as a middle click, which closed the menu about fifteen milliseconds after it opened
 * and then let the chord open it again, over and over. Measured on 2026-08-17: open at 01:39:11.949, closed
 * at 01:39:11.978, seven times in under a second.
 *
 * So the grip that opened it has to be spent before another one can close it. A short deadline does that
 * without needing to track which physical button is still down, which the controller state cannot tell us
 * anyway while the overlay owns the controllers.
 */
std::chrono::steady_clock::time_point s_shownAt{};
constexpr double kCloseGraceSeconds = 0.7;

// Set once when SteamVR refuses to play, so the failure is reported one time and never retried per frame.
bool s_unavailable = false;
bool s_visible = false;

Microsoft::WRL::ComPtr<ID3D11Texture2D> s_pTexture;
ID3D11Device* s_pDevice = nullptr;
ID3D11DeviceContext* s_pContext = nullptr;

// CEF paints on its own thread and D3D11 uploads happen on the render thread, so the page lands here first.
std::vector<uint8_t> s_pixels;
std::mutex s_pixelLock;
bool s_dirty = false;

// Defined below, declared here because the input pump closes the menu and sits above it.
void ApplyVisible(bool aVisible) noexcept;

bool AcquireInterface() noexcept
{
    if (s_pVROverlay)
        return true;

    if (s_unavailable)
        return false;

    // The game brings openvr_api.dll in and initialises it long before this runs, so this only borrows what
    // is already there. Resolved by hand rather than linked, because an import would have to resolve at
    // process start, when the dll is not yet anywhere the loader will look.
    HMODULE api = GetModuleHandleA("openvr_api.dll");
    if (!api)
    {
        spdlog::error("VR menu: openvr_api.dll is not loaded, the UI stays on the desktop window");
        s_unavailable = true;
        return false;
    }

    using TGetGenericInterface = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
    auto* pGetInterface = reinterpret_cast<TGetGenericInterface>(GetProcAddress(api, "VR_GetGenericInterface"));

    if (!pGetInterface)
    {
        spdlog::error("VR menu: openvr_api.dll has no VR_GetGenericInterface, the UI stays on the desktop window");
        s_unavailable = true;
        return false;
    }

    vr::EVRInitError error = vr::VRInitError_None;

    // Wanted for the controller buttons rather than for anything drawn. Not fatal if it is missing: the
    // overlay still works, only the A+grip and grip hotkeys stop.
    s_pVRSystem = static_cast<vr::IVRSystem*>(pGetInterface(vr::IVRSystem_Version, &error));
    if (!s_pVRSystem)
        spdlog::warn("VR menu: no {} ({}), the controller hotkeys will not work and F2 stays the only way in", vr::IVRSystem_Version, static_cast<int>(error));

    error = vr::VRInitError_None;
    s_pVROverlay = static_cast<vr::IVROverlay*>(pGetInterface(vr::IVROverlay_Version, &error));

    if (!s_pVROverlay)
    {
        // The version string is compiled in from the header. The installed runtime answers a long range of
        // them, so this failing means something bigger is wrong than a version mismatch.
        spdlog::error("VR menu: SteamVR would not hand over {} (error {}), the UI stays on the desktop window", vr::IVROverlay_Version, static_cast<int>(error));
        s_unavailable = true;
        return false;
    }

    return true;
}

bool EnsureOverlay() noexcept
{
    if (s_overlay != vr::k_ulOverlayHandleInvalid)
        return true;

    if (!AcquireInterface())
        return false;

    vr::EVROverlayError error = s_pVROverlay->CreateOverlay("skyrimtogether.menu", "Skyrim Together", &s_overlay);
    if (error != vr::VROverlayError_None)
    {
        spdlog::error("VR menu: CreateOverlay failed with {}", static_cast<int>(error));
        s_unavailable = true;
        return false;
    }

    s_pVROverlay->SetOverlayWidthInMeters(s_overlay, kWidthInMetres);

    // Mouse input, and interactive whenever it is on screen, which is what makes the laser appear and report
    // where it is pointing. The scale is in texture pixels so the events arrive in the page's own coordinates
    // and need no conversion beyond the flip below.
    s_pVROverlay->SetOverlayInputMethod(s_overlay, vr::VROverlayInputMethod_Mouse);
    s_pVROverlay->SetOverlayFlag(s_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

    const vr::HmdVector2_t cMouseScale{{static_cast<float>(kWidth), static_cast<float>(kHeight)}};
    s_pVROverlay->SetOverlayMouseScale(s_overlay, &cMouseScale);

    /**
     * Pinned to the headset rather than to the room.
     *
     * It follows the head, so it is always there to be read and never left behind by turning round, which for
     * a menu you open, use and close is the right trade. If it turns out to be uncomfortable to look at, the
     * alternative is to place it in the standing universe at the pose the head had when it opened, which
     * needs IVRSystem for the pose and is a change to this transform and nothing else.
     */
    vr::HmdMatrix34_t transform{};
    transform.m[0][0] = 1.f;
    transform.m[1][1] = 1.f;
    transform.m[2][2] = 1.f;
    transform.m[0][3] = kRightShiftInMetres;
    transform.m[2][3] = -kDistanceInMetres;

    s_pVROverlay->SetOverlayTransformTrackedDeviceRelative(s_overlay, vr::k_unTrackedDeviceIndex_Hmd, &transform);

    spdlog::info("VR menu: overlay created, {}x{} at {:.1f}m wide, {:.1f}m ahead, {:.2f}m right", kWidth, kHeight, kWidthInMetres, kDistanceInMetres, kRightShiftInMetres);

    // The overlay is built on the first frame that renders, which can be after the UI was already asked to
    // show itself. Without this the handle exists, the page is painted onto it, and it is never shown.
    if (s_visible)
        s_pVROverlay->ShowOverlay(s_overlay);

    return true;
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
        s_unavailable = true;
        return false;
    }

    return true;
}

// SteamVR reports where the laser is pointing as ordinary mouse events, which CEF already knows how to take.
void PumpInput() noexcept
{
    OverlayApp* pApp = World::Get().GetOverlayService().GetOverlayApp();
    if (!pApp)
        return;

    vr::VREvent_t event{};

    while (s_pVROverlay->PollNextOverlayEvent(s_overlay, &event, sizeof(event)))
    {
        // OpenVR's origin is the bottom left of the overlay and CEF's is the top left.
        const auto cX = static_cast<uint16_t>(event.data.mouse.x);
        const auto cY = static_cast<uint16_t>(kHeight - event.data.mouse.y);

        switch (event.eventType)
        {
        case vr::VREvent_MouseMove:
            pApp->InjectMouseMove(cX, cY, 0);
            break;

        /**
         * The trigger clicks the page, a grip closes the menu.
         *
         * A grip reaches the overlay as a middle click and cannot be read any other way: while the menu is up
         * SteamVR routes the controllers to it and the raw button state reads zero for both hands.
         */
        case vr::VREvent_MouseButtonDown:
        {
            if (event.data.mouse.button == vr::VRMouseButton_Left)
            {
                pApp->InjectMouseButton(cX, cY, MBT_LEFT, false, 0);
                break;
            }

            /**
             * Either hand closes it, because which hand pressed cannot be known here.
             *
             * Restricting this to the left grip was tried and cannot work through the overlay: SteamVR sends
             * these events with `trackedDeviceIndex` set to `k_unTrackedDeviceIndexInvalid`, measured as
             * 4294967295 in the log, so the event does not say which controller produced it. The raw
             * controller state cannot fill the gap either, since it reads zero for both hands while the
             * overlay owns them.
             *
             * Telling the hands apart needs the overlay to stop being interactive so the controllers report
             * normally, and this code to do the pointing itself with ComputeOverlayIntersection. Written up
             * in PROGRESS.md session 11.
             *
             * The grip that opened the menu is still held when the overlay appears and arrives here at once,
             * which closed it 29ms after it opened, so it has to be spent before another one can close.
             */
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - s_shownAt).count() < kCloseGraceSeconds)
                break;

            spdlog::info("VR menu: closing the menu");

            ApplyVisible(false);

            World::Get().GetRunner().Queue([]() { InputService::SetUI(false); });

            break;
        }

        case vr::VREvent_MouseButtonUp:
            if (event.data.mouse.button == vr::VRMouseButton_Left)
                pApp->InjectMouseButton(cX, cY, MBT_LEFT, true, 0);
            break;

        default: break;
        }
    }
}

// Raises or lowers the quad. The one place s_visible changes, so it can never say one thing while SteamVR
// shows another.
void ApplyVisible(bool aVisible) noexcept
{
    s_visible = aVisible;

    if (aVisible)
        s_shownAt = std::chrono::steady_clock::now();

    if (s_overlay == vr::k_ulOverlayHandleInvalid || !s_pVROverlay)
        return;

    if (aVisible)
        s_pVROverlay->ShowOverlay(s_overlay);
    else
        s_pVROverlay->HideOverlay(s_overlay);
}

/**
 * @brief A with a grip opens the menu.
 *
 * Legacy controller state rather than SteamVR's action system, because an action would mean shipping a
 * manifest and asking the player to bind it, and the game is a legacy input application so the runtime is
 * already serving that path. The masks were read off this client's own log rather than assumed.
 *
 * Only opening lives here. Once the menu is up SteamVR owns the controllers and this state reads zero for
 * both hands, so closing is handled in PumpInput where the input actually arrives. PROGRESS.md session 11.
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

    // Not constexpr: openvr's ButtonMaskFromId is a plain inline.
    const uint64_t cOpenMask = vr::ButtonMaskFromId(vr::k_EButton_A) | vr::ButtonMaskFromId(vr::k_EButton_Grip);

    // Either hand. Only opening is handled here; see PumpInput for why closing cannot be.
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
        if (s_unavailable || !EnsureOverlay() || !EnsureTexture())
            return;

        // Before the visibility check below, since its whole job is to be the way in when the menu is down.
        PollMenuChords();

        {
            std::scoped_lock lock(s_pixelLock);

            if (s_dirty)
            {
                s_pContext->UpdateSubresource(s_pTexture.Get(), 0, nullptr, s_pixels.data(), kWidth * 4, 0);
                s_dirty = false;
            }
        }

        if (!s_visible)
            return;

        vr::Texture_t texture{};
        texture.handle = s_pTexture.Get();
        texture.eType = vr::TextureType_DirectX;
        texture.eColorSpace = vr::ColorSpace_Auto;

        const vr::EVROverlayError cError = s_pVROverlay->SetOverlayTexture(s_overlay, &texture);

        if (cError != vr::VROverlayError_None)
        {
            // Reported once. If SteamVR will not take this texture the usual reason is the flags it was
            // created with, and every frame after the first says the same thing.
            static bool reported = false;
            if (!reported)
            {
                spdlog::error("VR menu: SetOverlayTexture failed with {}, the UI stays on the desktop window", static_cast<int>(cError));
                reported = true;
            }

            return;
        }

        PumpInput();
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

#endif
