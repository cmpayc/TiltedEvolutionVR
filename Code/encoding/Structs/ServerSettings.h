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

    // Whether every character except the one at this client stops colliding with loose objects. On is what
    // stops item drift: a body that is not this client's is moved by being teleported, and a teleport into an
    // object has havok eject it, which sends that object sliding across the room. On, NPCs and other players
    // still collide with you and can still be hit; they simply cannot shove clutter or a dropped weapon about.
    // Off leaves the game's own collision exactly as it is, drift included. Only the VR client acts on this.
    bool DisableCollisionBetweenOtherCharactersAndObjects{};

    // Whether pointing at another player is stopped from offering the activation prompt. On by default: there
    // is nothing a player can do with another one, and going through with it opens a dialogue that leads
    // nowhere. Off restores the vanilla behaviour, prompt and all. Only the VR client acts on this.
    bool BlockRemotePlayerActivation{true};
};
