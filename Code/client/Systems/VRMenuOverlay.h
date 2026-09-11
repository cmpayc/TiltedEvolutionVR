#pragma once

#if TP_SKYRIMVR

namespace TiltedPhoques
{
struct OverlayRenderHandler;
}

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

/**
 * @brief The mod's UI in the headset: owns the page, where it hangs, and how it is driven.
 *
 * The desktop path draws CEF onto the D3D11 swapchain, and in VR that swapchain is the mirror window, so the
 * page has to be put in front of the player some other way. It is painted into a texture of our own, which
 * VREyeOverlay then draws into the frame the game hands the compositor. Handing it to SteamVR as an overlay
 * instead was how this started, and it is gone: it needed IVROverlay, which an install running OpenVR through
 * a translation layer has no reason to serve, and the drawn one looked better anyway.
 *
 * Driving it has two modes, decided once by whether the runtime answers the IVRSystem version this file asks
 * for by name. Answered, and the controllers point at the page, their sticks move a cursor on it and their
 * triggers click it. Unanswered, and the page is still drawn and still placed, because none of that borrows
 * anything asked for by version, but the driving falls to the desktop mouse and keyboard.
 *
 * The mouse and keyboard work in either mode, which is what makes typing a server address possible.
 */
namespace VRMenuOverlay
{
/**
 * @brief The CEF render handler to use on VR, in place of the swapchain one.
 *
 * Owns the texture the page is painted into. Everything else here is driven from its Render, which the CEF
 * client calls once a frame on the render thread.
 */
TiltedPhoques::OverlayRenderHandler* CreateRenderHandler(ID3D11Device* apDevice, ID3D11DeviceContext* apContext) noexcept;

// Shown and hidden by the existing UI toggle, so F2 does the same thing it always did.
void SetVisible(bool aVisible) noexcept;

/**
 * @brief The page as it stands, for whoever is drawing it.
 *
 * One call rather than several, because a view without the size it is a view of, or without knowing whether
 * the menu is even up, is not enough to draw anything with.
 */
struct Page
{
    // Null until CEF has painted once and the texture exists.
    ID3D11ShaderResourceView* pView = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    // Whether the menu is up at all, and so whether there is anything to draw.
    bool draw = false;

    // Where it hangs, in metres, relative to the headset.
    float widthInMetres = 0.f;
    float distanceInMetres = 0.f;
    float rightShiftInMetres = 0.f;
    float upShiftInMetres = 0.f;

    // Where the cursor is, as a fraction across and down the page. False when nothing is driving it, which is
    // also when there is nothing to mark.
    bool cursorOnPage = false;
    float cursorAcross = 0.f;
    float cursorDown = 0.f;
};

Page GetPage() noexcept;

/**
 * @brief Where the desktop mouse is, as a fraction across and down the page.
 *
 * Ignored when there are controllers, since then they are what drives the menu and the marker follows them.
 * Without them this is the only thing that moves it, and the marker is the only way to see where it is: a
 * headset has no desktop cursor drawn over the page.
 */
void SetMouseCursor(float aAcross, float aDown) noexcept;

/**
 * @brief Whether the game should skip polling its input devices this frame.
 *
 * True only while the menu is up on the path that reads the controllers itself, and only once both hands have
 * been seen idle, so the game is never frozen holding a button. Asked from the game's own input hook.
 */
bool ShouldWithholdGameInput() noexcept;
} // namespace VRMenuOverlay

#endif
