#pragma once

/**
 * @brief Dispatched when a dropped item's world reference has been created locally.
 *
 * Items dropped from an inventory are temporary references, created independently on every client, so
 * their form ids differ per machine and are even recycled within one session. There is therefore nothing
 * about such an object that all clients agree on, which is why grabbing one could not be synced.
 *
 * The drop itself is the one moment every client shares. The dropping client mints an id for it and the
 * server relays it, so each client can pair "the object I just created" with the same id. That pairing is
 * what this event carries, and ObjectService keeps it.
 */
struct DynamicObjectCreatedEvent
{
    DynamicObjectCreatedEvent(const uint64_t aDropId, const uint32_t aFormId)
        : DropId(aDropId)
        , FormId(aFormId)
    {
    }

    uint64_t DropId;
    uint32_t FormId;
};
