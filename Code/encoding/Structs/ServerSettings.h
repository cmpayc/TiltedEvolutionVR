#pragma once

using TiltedPhoques::Buffer;

struct ServerSettings
{
    bool operator==(const ServerSettings& acRhs) const noexcept;
    bool operator!=(const ServerSettings& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    uint32_t Difficulty{};
    bool GreetingsEnabled{};
    bool PvpEnabled{};
    bool SyncPlayerHomes{};
    bool DeathSystemEnabled{};
    bool SyncPlayerCalendar{};
    bool AutoPartyJoin{};

    // Whether sleeping and waiting move the shared clock forward for everybody. Off means the clock only ever
    // advances at the server's own rate, which is what every build before this one did.
    bool SleepEnabled{};

    // Whether dead bodies are shared between clients. Living actors sync either way; this is only about
    // corpses. Off means a body is never assigned, never spawned from a remote request and never moved from
    // the network, so each client's own ragdoll settles it. A dead player is still a player and is unaffected.
    bool DeadBodySyncEnabled{};

    // Whether another player's body collides with the world. Off means it cannot push clutter about, and
    // cannot be hit by a thrown object either. Off by default: with it on, every object a remote body touched
    // slid across the room, because moving that body teleports its collision and havok resolves the overlap
    // by ejecting whatever it landed in. Measured on 2026-08-23, 23 drift events with it on and none with it
    // off over the same two minutes.
    bool RemoteBodyCollisionEnabled{};

    // Whether pointing at another player is stopped from offering the activation prompt. On by default: there
    // is nothing a player can do with another one, and going through with it opens a dialogue that leads
    // nowhere. Off restores the vanilla behaviour, prompt and all. Only the VR client acts on this.
    bool BlockRemotePlayerActivation{true};
};
