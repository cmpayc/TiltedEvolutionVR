#pragma once

#if TP_SKYRIMVR

namespace TiltedPhoques
{
struct OverlayRenderHandler;
}

struct ID3D11Device;
struct ID3D11DeviceContext;

/**
 * @brief Puts the mod's UI on a quad in the headset, driven by the same F2 toggle as before.
 *
 * The desktop path draws CEF onto the D3D11 swapchain, and in VR that swapchain is the mirror window on the
 * monitor, so the menu is visible to everyone except the person wearing the headset. Rather than fight the
 * game's own VR compositing, the page is painted into a texture of our own and handed to SteamVR as an
 * overlay, which puts it in front of the player and brings the laser pointer with it.
 *
 * Input needs no new plumbing. SteamVR reports controller interaction with an overlay as ordinary mouse
 * events in texture pixels, and CEF already accepts injected mouse events, so the two meet in the middle.
 * The desktop mouse and keyboard keep working exactly as they did, which is what makes typing a server
 * address possible until the SteamVR keyboard is wired up too.
 */
namespace VRMenuOverlay
{
/**
 * @brief The CEF render handler to use on VR, in place of the swapchain one.
 *
 * Owns the texture the page is painted into. Everything else here is driven from its Render, which the
 * overlay client calls once a frame on the render thread.
 */
TiltedPhoques::OverlayRenderHandler* CreateRenderHandler(ID3D11Device* apDevice, ID3D11DeviceContext* apContext) noexcept;

// Shown and hidden by the existing UI toggle, so F2 does the same thing it always did.
void SetVisible(bool aVisible) noexcept;
} // namespace VRMenuOverlay

#endif
