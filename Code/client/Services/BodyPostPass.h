#pragma once

// Post-pass re-apply: after the engine recomputes a body-owned skeleton (the BSFlattenedBoneTree's three
// downward-pass slots), the last pose the body renderer wrote is written back on the recomputing thread,
// before anything can read the engine's pose.
//
// This header is the pure part: the per-remote snapshot the main thread publishes, the lock-free triple
// buffer, and Reapply, which validates every address against the live tree it is handed (its entry array,
// its bone objects, their child lists) before writing. The hook install and the thunks are in
// BodyPostPass.cpp (VR only). Layout constants are the VR 1.4.15 offsets the body reader already uses.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

namespace BodyTracking::PostPass
{
constexpr size_t kTransformBytes = 0x34;
constexpr size_t kMaxSlots = 8;      // remotes with a published pose
constexpr size_t kMaxEntries = 1024; // the reader's own tree-entry cap
constexpr size_t kMaxItems = 64;     // written nodes outside the tree (grip item subtrees)
constexpr size_t kMaxChain = 6;      // native parent links from an entry object down to an item's parent
constexpr size_t kBuffers = 3;       // one writer, N readers: current, one possibly held, one free
constexpr size_t kMaxThreads = 8;    // distinct thread ids remembered per window
constexpr float kMovedUnits = 64.f;  // fresh tree root farther than this from the snapshot's = warped

constexpr uintptr_t kTreeCount = 0x150;    // BSFlattenedBoneTree: int32 entry count
constexpr uintptr_t kTreeData = 0x158;     // BSFlattenedBoneTree: entry array
constexpr uintptr_t kEntryStride = 0x80;   // one flattened entry
constexpr uintptr_t kEntryWorld = 0x34;    // entry world NiTransform
constexpr uintptr_t kEntryObject = 0x70;   // entry NiAVObject* (0 for object-less bones)
constexpr uintptr_t kEntryName = 0x78;     // entry name (BSFixedString data pointer)
constexpr uintptr_t kObjectWorld = 0x7c;   // NiAVObject world NiTransform
constexpr uintptr_t kWorldTranslation = 0x24; // float[3] inside a NiTransform (after the 3x3)
constexpr uintptr_t kNodeChildren = 0x140; // NiNode child array (NiAVObject*[])
constexpr uintptr_t kNodeChildEnd = 0x14a; // NiNode child array end (uint16)
constexpr uint32_t kMaxChildren = 1024;

struct RawTransform
{
    std::array<uint8_t, kTransformBytes> Bytes{};
};
struct EntryWrite
{
    uint32_t Index{};   // flattened entry index
    uintptr_t Object{}; // the entry's object at publish time (0 = object-less bone)
    uintptr_t Name{};   // the entry's name pointer at publish time
    bool ObjectWrite{true}; // false when the entry's object is the tree itself: only entry+0x34 is restored
    RawTransform World;
};
struct ItemWrite
{
    uint32_t EntryIndex{}; // flattened entry whose object heads the chain
    uint32_t ChainLength{};
    std::array<uintptr_t, kMaxChain> Chain{}; // entry object, then each native child down to the item's parent
    std::array<uintptr_t, kMaxChain> ChainVtables{}; // each chain node's vtable at publish time: pins NiNode layout before +0x140 is read
    uintptr_t Object{};
    RawTransform World;
};
// What one Apply wrote, expressed in tree terms. Built by NativeBody::Apply; no engine pointer in it is ever
// dereferenced without the live-tree checks in Reapply.
struct Snapshot
{
    uintptr_t Tree{}, Data{};
    uint32_t Count{};
    uint32_t TreeCount{}; // flattened trees the reader found; only the one with the most written entries is published
    bool MultiTree{};     // owned writes spanned more than one tree this frame: the others are main-thread-only (unsupported)
    std::array<float, 3> RootPosition{}; // tree world translation at write time (not written back)
    std::vector<EntryWrite> Entries;
    std::vector<ItemWrite> Items;
    uint32_t Unmapped{}; // written addresses that are neither entry nor a chained item
};

struct ReapplyStats
{
    uint32_t Writes{}, Skips{};
    bool Matched{}, Stale{}, Moved{}, Busy{};
};
struct WindowCounters
{
    uint64_t Calls{}, Writes{}, Skips{}, Stale{}, Moved{}, Busy{}, Overlap{}, MaxMicros{};
    uint64_t Reapplies{}; // passes that re-applied the whole published pose (not stale, not moved, something written)
    uint32_t Threads{};
    uint64_t Publishes{}, PublishBusy{}, Unmapped{};
};

enum class PublishResult { Published, NoSlot, TooLarge, Busy };

struct Buffer
{
    uintptr_t Tree{}, Data{};
    uint32_t Count{};
    std::array<float, 3> RootPosition{};
    uint32_t EntryCount{}, ItemCount{};
    std::array<EntryWrite, kMaxEntries> Entries{};
    std::array<ItemWrite, kMaxItems> Items{};
};
struct Slot
{
    std::atomic<bool> Used{false};
    std::atomic<uint32_t> Remote{0};
    std::atomic<uintptr_t> Tree{0};
    std::atomic<uint32_t> Current{0};
    std::array<std::atomic<uint32_t>, kBuffers> Readers{};
    std::atomic<uint32_t> Depth{0}; // thunks inside the engine pass or Reapply for this slot right now (see BeginPass)
    std::array<Buffer, kBuffers> Buffers{};
    // Window counters, reset by TakeCounters on the logging thread.
    std::atomic<uint64_t> Calls{0}, Writes{0}, Skips{0}, Stale{0}, Moved{0}, Busy{0}, Overlap{0}, MaxMicros{0}, Reapplies{0};
    std::atomic<uint64_t> Publishes{0}, PublishBusy{0}, Unmapped{0};
    std::array<std::atomic<uint32_t>, kMaxThreads> Threads{};
};

// Static storage in the process; nothing here is ever freed, because a thunk may be inside Reapply at any time.
class Registry
{
public:
    PublishResult Publish(uint32_t aRemote, const Snapshot& aSnapshot) noexcept
    {
        Slot* pSlot = Find(aRemote);
        if (!pSlot)
        {
            for (auto& slot : m_slots)
            {
                // A released slot is not reused while a thunk that entered it before the release is still inside, so
                // that thunk's bracket can only ever balance the slot it entered.
                if (slot.Depth.load(std::memory_order_acquire) != 0) continue;
                bool expected = false;
                if (slot.Used.compare_exchange_strong(expected, true))
                {
                    ResetWindow(slot); // a recycled slot must not report its previous occupant's window
                    slot.Remote.store(aRemote, std::memory_order_release);
                    pSlot = &slot;
                    break;
                }
            }
            if (!pSlot) return PublishResult::NoSlot;
        }
        if (aSnapshot.Entries.size() > kMaxEntries || aSnapshot.Items.size() > kMaxItems) return PublishResult::TooLarge;
        const uint32_t current = pSlot->Current.load(std::memory_order_acquire);
        uint32_t target = kBuffers;
        for (uint32_t j = 0; j < kBuffers; ++j)
            if (j != current && pSlot->Readers[j].load(std::memory_order_acquire) == 0) { target = j; break; }
        if (target == kBuffers)
        {
            pSlot->PublishBusy.fetch_add(1, std::memory_order_relaxed);
            return PublishResult::Busy;
        }
        auto& buffer = pSlot->Buffers[target];
        buffer.Tree = aSnapshot.Tree;
        buffer.Data = aSnapshot.Data;
        buffer.Count = aSnapshot.Count;
        buffer.RootPosition = aSnapshot.RootPosition;
        buffer.EntryCount = static_cast<uint32_t>(aSnapshot.Entries.size());
        buffer.ItemCount = static_cast<uint32_t>(aSnapshot.Items.size());
        for (size_t i = 0; i < aSnapshot.Entries.size(); ++i) buffer.Entries[i] = aSnapshot.Entries[i];
        for (size_t i = 0; i < aSnapshot.Items.size(); ++i) buffer.Items[i] = aSnapshot.Items[i];
        // A reader that registered on `target` before this fill re-checks Current, which cannot equal `target`
        // until the store below; the release on Current publishes the fill to any reader that then passes.
        pSlot->Tree.store(aSnapshot.Tree, std::memory_order_release);
        pSlot->Current.store(target, std::memory_order_release);
        pSlot->Publishes.fetch_add(1, std::memory_order_relaxed);
        pSlot->Unmapped.fetch_add(aSnapshot.Unmapped, std::memory_order_relaxed);
        return PublishResult::Published;
    }

    void Release(uint32_t aRemote) noexcept
    {
        if (Slot* pSlot = Find(aRemote)) Clear(*pSlot);
    }

    // A held buffer: what a thunk holds while it re-applies, exposed so tests can occupy the spares.
    struct Hold
    {
        Slot* SlotPtr{};
        uint32_t Buffer{kBuffers};
        bool Held() const noexcept { return SlotPtr && Buffer < kBuffers; }
    };
    Hold AcquireHold(uint32_t aRemote) noexcept
    {
        Hold hold;
        if (Slot* pSlot = Find(aRemote)) { hold.SlotPtr = pSlot; hold.Buffer = AcquireBuffer(*pSlot); }
        return hold;
    }
    void ReleaseHold(Hold& aHold) noexcept
    {
        if (aHold.Held()) aHold.SlotPtr->Readers[aHold.Buffer].fetch_sub(1, std::memory_order_acq_rel);
        aHold = {};
    }
    void ReleaseAll() noexcept
    {
        for (auto& slot : m_slots) if (slot.Used.load(std::memory_order_acquire)) Clear(slot);
    }

    // Thunk bracket: called before the engine's own pass and after Reapply, so `overlap` counts a thunk that
    // enters while another thunk (engine pass or re-apply) is still inside the same tree. Returns a token naming the
    // slot that was entered (its index plus one), 0 when the tree is not owned; EndPass balances exactly that slot,
    // whatever happened to it in between, so a release during a pass or two slots in use at once cannot unbalance
    // another slot's count.
    uint32_t BeginPass(uintptr_t aTree) noexcept
    {
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            auto& slot = m_slots[i];
            if (!slot.Used.load(std::memory_order_acquire) || slot.Tree.load(std::memory_order_acquire) != aTree) continue;
            if (slot.Depth.fetch_add(1, std::memory_order_acq_rel) != 0) slot.Overlap.fetch_add(1, std::memory_order_relaxed);
            return static_cast<uint32_t>(i + 1);
        }
        return 0;
    }
    void EndPass(uint32_t aToken) noexcept
    {
        if (aToken == 0 || aToken > m_slots.size()) return;
        m_slots[aToken - 1].Depth.fetch_sub(1, std::memory_order_acq_rel);
    }

    // Any thread. Memory: bool Load(uintptr_t, T&) const and void Store(uintptr_t, const RawTransform&).
    // Returns whether aTree matched a slot; stats say what happened to it.
    template <class Memory> bool Reapply(uintptr_t aTree, Memory& aMemory, ReapplyStats& aStats, uint32_t aThreadId = 0) noexcept
    {
        aStats = {};
        for (auto& slot : m_slots)
        {
            if (!slot.Used.load(std::memory_order_acquire) || slot.Tree.load(std::memory_order_acquire) != aTree) continue;
            aStats.Matched = true;
            slot.Calls.fetch_add(1, std::memory_order_relaxed);
            if (aThreadId) NoteThread(slot, aThreadId);
            const uint32_t held = AcquireBuffer(slot);
            if (held == kBuffers)
            {
                aStats.Busy = true;
                slot.Busy.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                Apply(slot.Buffers[held], aTree, aMemory, aStats);
                slot.Readers[held].fetch_sub(1, std::memory_order_acq_rel);
                slot.Writes.fetch_add(aStats.Writes, std::memory_order_relaxed);
                slot.Skips.fetch_add(aStats.Skips, std::memory_order_relaxed);
                if (aStats.Stale) slot.Stale.fetch_add(1, std::memory_order_relaxed);
                if (aStats.Moved) slot.Moved.fetch_add(1, std::memory_order_relaxed);
                if (!aStats.Stale && !aStats.Moved && aStats.Writes) slot.Reapplies.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        return false;
    }

    void NoteMicros(uintptr_t aTree, uint64_t aMicros) noexcept
    {
        for (auto& slot : m_slots)
        {
            if (!slot.Used.load(std::memory_order_acquire) || slot.Tree.load(std::memory_order_acquire) != aTree) continue;
            uint64_t previous = slot.MaxMicros.load(std::memory_order_relaxed);
            while (aMicros > previous && !slot.MaxMicros.compare_exchange_weak(previous, aMicros, std::memory_order_relaxed)) {}
            return;
        }
    }

    // Logging thread: read and reset this remote's window. False when the remote has no slot.
    bool TakeCounters(uint32_t aRemote, WindowCounters& aOut) noexcept
    {
        Slot* pSlot = Find(aRemote);
        if (!pSlot) return false;
        aOut.Calls = pSlot->Calls.exchange(0, std::memory_order_relaxed);
        aOut.Writes = pSlot->Writes.exchange(0, std::memory_order_relaxed);
        aOut.Skips = pSlot->Skips.exchange(0, std::memory_order_relaxed);
        aOut.Stale = pSlot->Stale.exchange(0, std::memory_order_relaxed);
        aOut.Moved = pSlot->Moved.exchange(0, std::memory_order_relaxed);
        aOut.Busy = pSlot->Busy.exchange(0, std::memory_order_relaxed);
        aOut.Overlap = pSlot->Overlap.exchange(0, std::memory_order_relaxed);
        aOut.MaxMicros = pSlot->MaxMicros.exchange(0, std::memory_order_relaxed);
        aOut.Reapplies = pSlot->Reapplies.exchange(0, std::memory_order_relaxed);
        aOut.Publishes = pSlot->Publishes.exchange(0, std::memory_order_relaxed);
        aOut.PublishBusy = pSlot->PublishBusy.exchange(0, std::memory_order_relaxed);
        aOut.Unmapped = pSlot->Unmapped.exchange(0, std::memory_order_relaxed);
        aOut.Threads = 0;
        for (auto& thread : pSlot->Threads) if (thread.exchange(0, std::memory_order_relaxed)) ++aOut.Threads;
        return true;
    }

    size_t UsedSlots() const noexcept
    {
        size_t used = 0;
        for (const auto& slot : m_slots) if (slot.Used.load(std::memory_order_acquire)) ++used;
        return used;
    }

private:
    Slot* Find(uint32_t aRemote) noexcept
    {
        for (auto& slot : m_slots)
            if (slot.Used.load(std::memory_order_acquire) && slot.Remote.load(std::memory_order_acquire) == aRemote) return &slot;
        return nullptr;
    }
    // Register on the current buffer, then re-check that it is still current (the writer never flips onto a
    // buffer with readers, so a passed re-check means the fill completed before we could see it). Three tries.
    static uint32_t AcquireBuffer(Slot& aSlot) noexcept
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const uint32_t current = aSlot.Current.load(std::memory_order_acquire);
            aSlot.Readers[current].fetch_add(1, std::memory_order_acq_rel);
            if (const auto probe = AcquireProbe.load(std::memory_order_relaxed)) probe(); // tests only: interleave a publish here
            if (aSlot.Current.load(std::memory_order_acquire) == current) return current;
            aSlot.Readers[current].fetch_sub(1, std::memory_order_acq_rel);
        }
        return kBuffers;
    }
    static void Clear(Slot& aSlot) noexcept
    {
        // Order: the tree stops matching first, so no new reader enters; a reader already inside holds its
        // buffer by Readers[] and finishes on memory the tree it validated still owns. Buffers, reader counts and
        // depth are not wiped; the window counters are, so nothing carries over to the next occupant.
        aSlot.Tree.store(0, std::memory_order_release);
        aSlot.Remote.store(0, std::memory_order_release);
        ResetWindow(aSlot);
        aSlot.Used.store(false, std::memory_order_release);
    }
    static void ResetWindow(Slot& aSlot) noexcept
    {
        for (auto* counter : {&aSlot.Calls, &aSlot.Writes, &aSlot.Skips, &aSlot.Stale, &aSlot.Moved, &aSlot.Busy, &aSlot.Overlap, &aSlot.MaxMicros, &aSlot.Reapplies, &aSlot.Publishes, &aSlot.PublishBusy, &aSlot.Unmapped})
            counter->store(0, std::memory_order_relaxed);
        for (auto& thread : aSlot.Threads) thread.store(0, std::memory_order_relaxed);
    }
    static void NoteThread(Slot& aSlot, uint32_t aThreadId) noexcept
    {
        for (auto& thread : aSlot.Threads)
        {
            uint32_t expected = 0;
            const uint32_t seen = thread.load(std::memory_order_relaxed);
            if (seen == aThreadId) return;
            if (seen == 0 && thread.compare_exchange_strong(expected, aThreadId, std::memory_order_relaxed)) return;
        }
    }
    template <class Memory> static bool Listed(Memory& aMemory, uintptr_t aParent, uintptr_t aChild) noexcept
    {
        uintptr_t children{};
        uint16_t end{};
        if (!aParent || !aChild || !aMemory.Load(aParent + kNodeChildren, children) || !aMemory.Load(aParent + kNodeChildEnd, end) ||
            !children || end > kMaxChildren) return false;
        for (uint32_t c = 0; c < end; ++c)
        {
            uintptr_t child{};
            if (!aMemory.Load(children + c * sizeof(uintptr_t), child)) return false;
            if (child == aChild) return true;
        }
        return false;
    }
    template <class Memory> static void Apply(const Buffer& aBuffer, uintptr_t aTree, Memory& aMemory, ReapplyStats& aStats) noexcept
    {
        uint32_t count{};
        uintptr_t data{};
        std::array<float, 3> root{};
        if (aBuffer.Tree != aTree || !aMemory.Load(aTree + kTreeCount, count) || !aMemory.Load(aTree + kTreeData, data) ||
            count != aBuffer.Count || data != aBuffer.Data || !data || !aMemory.Load(aTree + kObjectWorld + kWorldTranslation, root))
        {
            aStats.Stale = true;
            aStats.Skips += aBuffer.EntryCount + aBuffer.ItemCount;
            return;
        }
        float distance = 0.f;
        for (int i = 0; i < 3; ++i) distance += (root[i] - aBuffer.RootPosition[i]) * (root[i] - aBuffer.RootPosition[i]);
        if (!(distance <= kMovedUnits * kMovedUnits)) // NaN counts as moved
        {
            aStats.Moved = true;
            aStats.Skips += aBuffer.EntryCount + aBuffer.ItemCount;
            return;
        }
        for (uint32_t i = 0; i < aBuffer.EntryCount; ++i)
        {
            const auto& entry = aBuffer.Entries[i];
            uintptr_t object{}, name{};
            if (entry.Index >= count) { ++aStats.Skips; continue; }
            const uintptr_t address = data + entry.Index * kEntryStride;
            if (!aMemory.Load(address + kEntryObject, object) || object != entry.Object ||
                !aMemory.Load(address + kEntryName, name) || name != entry.Name) { ++aStats.Skips; continue; }
            aMemory.Store(address + kEntryWorld, entry.World);
            if (object && entry.ObjectWrite) aMemory.Store(object + kObjectWorld, entry.World);
            ++aStats.Writes;
        }
        for (uint32_t i = 0; i < aBuffer.ItemCount; ++i)
        {
            const auto& item = aBuffer.Items[i];
            if (item.EntryIndex >= count || !item.Object || !item.ChainLength || item.ChainLength > kMaxChain) { ++aStats.Skips; continue; }
            uintptr_t head{};
            if (!aMemory.Load(data + item.EntryIndex * kEntryStride + kEntryObject, head) || head != item.Chain[0]) { ++aStats.Skips; continue; }
            // Walk outward. A node is dereferenced only after the node before it (alive by construction) still lists
            // it as a child; its vtable must then be what the reader saw, so its +0x140/+0x14a are an NiNode's child
            // array and not another class's bytes. A detached, freed node is never read.
            bool linked = true;
            for (uint32_t link = 0; link < item.ChainLength && linked; ++link)
            {
                uintptr_t vtable{};
                if (link) linked = Listed(aMemory, item.Chain[link - 1], item.Chain[link]);
                linked = linked && aMemory.Load(item.Chain[link], vtable) && vtable == item.ChainVtables[link];
            }
            if (!linked || !Listed(aMemory, item.Chain[item.ChainLength - 1], item.Object)) { ++aStats.Skips; continue; }
            aMemory.Store(item.Object + kObjectWorld, item.World);
            ++aStats.Writes;
        }
    }

    std::array<Slot, kMaxSlots> m_slots{};
public:
    // Test-only interleaving hook between a reader's registration and its re-check; never set in the client.
    static inline std::atomic<void (*)()> AcquireProbe{nullptr};
};

// Snapshot construction helper: classify one written world address against the tree the reader found.
// Returns 0 = the tree's own world (never republished), 1 = entry (aIndex set), 2 = not in the tree.
inline int ClassifyAddress(uintptr_t aAddress, uintptr_t aTree, uintptr_t aData, uint32_t aCount, uint32_t& aIndex) noexcept
{
    if (aAddress == aTree + kObjectWorld) return 0;
    if (aData && aAddress >= aData + kEntryWorld && aAddress < aData + static_cast<uintptr_t>(aCount) * kEntryStride)
    {
        const uintptr_t offset = aAddress - aData;
        if (offset % kEntryStride == kEntryWorld)
        {
            aIndex = static_cast<uint32_t>(offset / kEntryStride);
            return 1;
        }
    }
    return 2;
}

#if TP_SKYRIMVR
// BodyPostPass.cpp: the process-wide registry, the pinned install, and the per-second ownership check.
Registry& Global() noexcept;
bool Installed() noexcept;
bool Active() noexcept; // installed and no slot lost since
bool SlotsIntact() noexcept;
void RegisterAtStartup() noexcept;
#endif
} // namespace BodyTracking::PostPass
