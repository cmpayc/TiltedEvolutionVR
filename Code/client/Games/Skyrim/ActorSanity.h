#pragma once

struct Actor;

/**
 * @brief Reports an actor whose own pointer fields no longer hold plausible pointers.
 *
 * Three of the four crashes on 2026-08-18 were the game reading a corrupted pointer out of actor or AI data,
 * all of them reached from TESConditionItem::IsTrue, and each time a different field:
 *
 *   - Actor+0xB0, the ActorValueOwner vtable, held 0x60000000 and then a heap pointer.
 *   - ActorValueStorage+0x08, the value array, held 0x10010FB32.
 *   - an AI package field at +0x40 held a vtable address where an object pointer belongs, and the object
 *     owning it had 0x141E72B24 stored with its top four bytes zeroed.
 *
 * Every one of those values is caught by cheap arithmetic, with no page queries: a real pointer here is
 * 8 byte aligned, canonically addressed, and not in the first 64KB, and a vtable additionally lives inside
 * the game image. Two of the four were misaligned, one was non-canonical, one was outside the image.
 *
 * The point is to name the actor and the field while it is still only data, rather than minutes later inside
 * the game's own code where the dump cannot say who wrote it.
 *
 * @param apActor actor to check, may be null.
 * @param acpWhere call site label, so the log says which of our operations the actor came through.
 * @return true when every field checked still looks like what it is supposed to be.
 */
bool ValidateActor(Actor* apActor, const char* acpWhere) noexcept;
