#pragma once

/**
 * @brief Dispatched when a VR hand takes hold of a world object, or lets go of one.
 *
 * Carries a form id rather than a reference because HIGGS reports hand events from its own threads and
 * HiggsService defers them to the update before dispatching. See Services/Generic/HiggsService.cpp.
 *
 * Only ever dispatched for static references. A temporary one names nothing on another client, so
 * there is no point telling anyone about it.
 */
struct ObjectHoldEvent
{
    ObjectHoldEvent(const uint32_t aFormId, const bool aIsLeft, const bool aIsReleased)
        : FormId(aFormId)
        , IsLeft(aIsLeft)
        , IsReleased(aIsReleased)
    {
    }

    uint32_t FormId;
    bool IsLeft;
    bool IsReleased;
};
