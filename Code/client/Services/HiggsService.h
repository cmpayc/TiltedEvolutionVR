#pragma once

struct UpdateEvent;

/**
 * @brief Bridges HIGGS, the SkyrimVR hand physics mod, into the client.
 *
 * HIGGS is loaded by SKSE, which the client only starts after building the world, so the interface
 * cannot be resolved in the constructor. This polls for it instead and subscribes to HIGGS's hand
 * events once, the first time it succeeds.
 *
 * HIGGS calls back from whatever thread it is on, so the callbacks only queue and the work happens
 * on the update. See the comment on Enqueue in the cpp.
 *
 * **This service is the only gate on hand sync, and the gate is structural rather than a flag.** It is
 * the only thing that dispatches ObjectHoldEvent, and it only does so from a HIGGS callback. No HIGGS
 * means no callbacks, which means no hold events, which means ObjectService never streams anything. So
 * there is no need for an "is HIGGS present" check anywhere else, and adding one would only be a second
 * source of truth. Keep it that way.
 *
 * Receiving is deliberately **not** gated: a client with no HIGGS, including a desktop SE client, still
 * applies incoming object transforms and so still sees what a VR player is carrying. That path writes a
 * position and calls Update3DPosition, neither of which involves HIGGS.
 *
 * Does nothing at all on a Skyrim SE build.
 */
struct HiggsService
{
    explicit HiggsService(entt::dispatcher& aDispatcher);
    ~HiggsService() noexcept = default;

    TP_NOCOPYMOVE(HiggsService);

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

    // Looks for HIGGS until it is found or the attempt budget runs out.
    void Resolve(double aDelta) noexcept;

    entt::scoped_connection m_updateConnection;

    double m_sinceLastAttempt{0.0};
    uint32_t m_attempts{0};
    bool m_settled{false};
};
