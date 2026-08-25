#pragma once

#include "Message.h"

struct RequestDeathStateChange final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDeathStateChange;

    RequestDeathStateChange()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDeathStateChange& acRhs) const noexcept { return Id == acRhs.Id && IsDead == acRhs.IsDead && IsBleedingOut == acRhs.IsBleedingOut && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id;
    bool IsDead;

    /**
     * @brief Whether the actor is down but not dead, which is the only state its owner can report about it.
     *
     * An essential actor does not die when it runs out of health. It drops into bleedout and gets up again, and
     * IsDead is false for the whole of that, so a message carrying only IsDead says nothing at all about the
     * one thing another client can see: whether the body is on the floor.
     *
     * Without this, an observing client has to infer it, and the only thing it has to infer from is the health
     * it was sent. That inference does not work. Health arriving at zero makes that client's own game drop the
     * body into a bleedout of its own, and coming out of one is a decision the game takes from combat state
     * that a remote actor never runs, so the body stays down for good however the health then moves. Measured
     * on 2026-08-25: the owner's NPC recovered and stood up, the observer's copy lay on the floor for the rest
     * of the session.
     *
     * With it the owner simply says which of the two it is and the observer mirrors it, in both directions.
     */
    bool IsBleedingOut{};
};
