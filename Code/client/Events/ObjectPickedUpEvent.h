#pragma once

/**
 * @brief Dispatched when a world object is taken into an inventory locally.
 *
 * Only interesting for a dropped item. Its world reference exists separately on every client, so the others
 * cannot tell from an inventory change alone which object left the world, and they leave it lying on the floor.
 * ObjectService owns the pairing between a reference and its drop, so it decides what to tell them.
 */
struct ObjectPickedUpEvent
{
    ObjectPickedUpEvent(const uint32_t aFormId, const uint32_t aBaseFormId)
        : FormId(aFormId)
        , BaseFormId(aBaseFormId)
    {
    }

    uint32_t FormId;

    /**
     * @brief The base form, carried because by the time this is handled the reference is gone.
     *
     * Handling is deferred to the update, and the pickup destroys the object in the meantime, so the registry
     * cannot be asked whether the reference is still valid: it never is. The base form is what stands in for
     * that check, since temporary form ids are recycled and matching on the id alone could name a different
     * object entirely.
     */
    uint32_t BaseFormId;
};
