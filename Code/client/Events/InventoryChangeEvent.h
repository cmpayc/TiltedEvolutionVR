#pragma once

#include <Structs/Inventory.h>
#include <ExtraData/ExtraDataList.h>

/**
 * @brief Dispatched when the contents of an object or actor inventory changes locally.
 *
 * The event has a Drop member variable, since dropped items need to be handled differently.
 */
struct InventoryChangeEvent
{
    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem)
        : FormId(aFormId)
        , Item(std::move(arItem))
    {
    }

    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem, bool aDrop)
        : FormId(aFormId)
        , Item(std::move(arItem))
        , Drop(aDrop)
    {
    }

    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem, bool aDrop, bool aUpdateClients)
        : FormId(aFormId)
        , Item(std::move(arItem))
        , Drop(aDrop)
        , UpdateClients(aUpdateClients)
    {
    }

    uint32_t FormId{};
    Inventory::Entry Item{};
    bool Drop = false;
    bool UpdateClients = true;

    /**
     * @brief The raw handle of the world reference a drop created, or zero.
     *
     * A handle rather than a form id on purpose. Turning one into the other means calling into the game, and
     * doing that inside the drop hook, while the game is still in the middle of creating the reference, left
     * the dropper with an item that hung in the air and never moved again. Measured: the dropper's own copy
     * kept the position it was created at while every other client's copy fell normally.
     *
     * So the hook only copies the number out, and the resolve happens a frame later once the drop has
     * finished. See HookDropObject and InventoryService::OnInventoryChangeEvent.
     */
    uint32_t DroppedHandle{};
};
