#pragma once

struct OverlayService;

/**
 * @brief Handles input handling for the UI.
 */
struct InputService
{
    InputService(OverlayService& aOverlay) noexcept;
    ~InputService() noexcept;

    static LRESULT WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    /**
     * @brief Raises or lowers the UI, doing everything the F2 key does.
     *
     * Exposed because F2 is not the only way to ask for the menu any more: on VR it is opened and closed by
     * controller buttons, and those must go through the same door, input capture and cursor included, rather
     * than only making the page visible.
     *
     * Set rather than toggle on purpose. The VR hotkeys are one button for open and a different one for
     * close, so each says what it wants and neither has to agree with anyone else about the current state.
     */
    static void SetUI(bool aActive) noexcept;

    TP_NOCOPYMOVE(InputService);
};
