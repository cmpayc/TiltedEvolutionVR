#pragma once

#if TP_SKYRIMVR

/**
 * @brief Puts the menu in the headset without asking the runtime for an overlay.
 *
 * Handing a texture to IVROverlay and letting the compositor place it was how this started. That interface is
 * the one thing a SkyrimVR install can plausibly lack: the game asks for IVROverlay_018 and IVRCompositor_022,
 * we would ask for IVROverlay_027, and only a full SteamVR install answers every version ever shipped. A
 * translation layer serving OpenXR answers what games ask for and no more.
 *
 * What no install can lack is the compositor, because the game cannot draw a frame without it. So this draws
 * the page into the frame on its way past: the game submits one 3648x1968 texture twice, left eye in the left
 * half and right eye in the right, and a quad goes into each half before the compositor sees it.
 *
 * There is nothing to install from the outside. The interface the game is handed is caught as it is handed
 * over, which is also the only moment the version it asked for is known.
 */
namespace VREyeOverlay
{
} // namespace VREyeOverlay

#endif
