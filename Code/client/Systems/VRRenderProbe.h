#pragma once

#if TP_SKYRIMVR

/**
 * @brief Logs the renderer's targets, for when something about the frame stops adding up.
 *
 * This was written to find where the headset's image lives, and the answer turned out to be that it does not
 * live in this table at all: the game resolves into a texture of its own before submitting, and the target
 * bound at the frame end hook is an unrelated 1024x1024 one. VREyeOverlay takes the texture from the submit
 * call for that reason.
 *
 * Kept because a game update reshuffling these is exactly the kind of thing that would send someone looking
 * again. It writes to the log and changes no game state.
 */
namespace VRRenderProbe
{
/**
 * @brief Censuses the renderer's targets, once at the main menu and once with the player in the world.
 *
 * Called from the frame end hook, which is the point a fallback overlay would have to draw from, so what it
 * reports about the bound target is what that overlay would be handed.
 */
void OnFrame() noexcept;
} // namespace VRRenderProbe

#endif
