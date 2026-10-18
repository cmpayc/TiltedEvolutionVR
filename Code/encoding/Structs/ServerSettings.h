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

    // Whether every character except the one at this client stops colliding with loose objects. On is what
    // stops item drift: a body that is not this client's is moved by being teleported, and a teleport into an
    // object has havok eject it, which sends that object sliding across the room. On, NPCs and other players
    // still collide with you and can still be hit; they simply cannot shove clutter or a dropped weapon about.
    // Off leaves the game's own collision exactly as it is, drift included. Only the VR client acts on this.
    bool DisableCollisionBetweenOtherCharactersAndObjects{};

    // Whether a body another client owns ragdolls and settles by itself here once it is dead: the game may move,
    // turn and process it. Off keeps the body pinned the way every build before this one had it. The death, its
    // animation and the dead state reach every client either way. Only the VR client acts on this.
    bool NotOwnedDeadBodyRagdoll{true};

    // Whether pointing at another player is stopped from offering the activation prompt. On by default: there
    // is nothing a player can do with another one, and going through with it opens a dialogue that leads
    // nowhere. Off restores the vanilla behaviour, prompt and all. Only the VR client acts on this.
    bool BlockRemotePlayerActivation{true};
};
