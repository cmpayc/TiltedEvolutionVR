#pragma once

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;

/**
 * @brief Keeps every character except this client's own player from shoving loose objects about.
 *
 * This is the item drift fix. A body that is not this client's is moved by being teleported, by
 * `InterpolationSystem`, and a teleport into an object has havok resolve the overlap by ejecting whatever it
 * landed in, which sends that object sliding across the room and never stopping. The contact is removed rather
 * than rationed: rate capping the warps was tried and changed nothing.
 *
 * It works through Skyrim's layer-versus-layer collision table, a bitfield per collision layer naming the layers
 * it meets, so one write covers every actor in the game rather than each body as it spawns. That is also its
 * limit, because the table is per layer and not per body, and every character in the game shares those layers.
 * So the local player, whose collision is the one that is never teleported and therefore never ejects anything,
 * is given mirror layers of their own: unused rows holding a copy of each character row, which lets exactly one
 * character back through the pairs the table now closes.
 *
 * What that adds up to is the behaviour asked for: the player collides with everything, and an NPC or another
 * player's body collides with the player and can still be hit, but cannot move the clutter or the dropped
 * weapons in the room.
 *
 * Governed by the server's `bDisableCollisionBetweenOtherCharactersAndObjects`, off by default, so a server that
 * says nothing gets the game exactly as it shipped, drift included.
 *
 * Does nothing at all on a Skyrim SE build.
 */
struct CollisionService
{
    CollisionService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~CollisionService() noexcept = default;

    TP_NOCOPYMOVE(CollisionService);

private:
    /**
     * @brief Decides, once per frame, whether anything needs doing, and almost always decides that it does not.
     *
     * Everything expensive here reads unverified game memory through `IsReadable`, which is a `VirtualQuery` and
     * therefore a syscall. An earlier version of this resolved the player's collision on every frame; the resolve
     * failed, failure is the case that falls through to a search, and a thousand syscalls a frame at 72Hz made
     * the game unplayable. So the frame path is made only of reads that cost nothing, and the work runs when one
     * of them says the answer can have changed.
     */
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

    // Puts the player's own layers back. A mirror row only means something in the table of a world this patched.
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;

    // What the last pass saw. Each of these changes which bodies the player has, or which physics world they are
    // in, and each is a plain load: a pointer, a pointer, a flag, a float.
    const void* m_root{nullptr};
    const void* m_cell{nullptr};
    bool m_weaponDrawn{false};
    double m_sinceLastPass{0.0};
};
