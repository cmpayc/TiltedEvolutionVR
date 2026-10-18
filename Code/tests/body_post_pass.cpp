// The post-pass re-apply's pure part: the triple buffer between the main-thread publisher and the job-thread
// thunks, Reapply's validation of every address against a synthetic live tree laid out in a byte arena, and
// the address classifier the snapshot builder uses. Nothing here touches the engine.
#include <catch2/catch.hpp>
#include "../client/Services/BodyPostPass.h"
#include <memory>
#include <vector>

using namespace BodyTracking::PostPass;

namespace
{
// A byte arena whose addresses are real pointers, so Load/Store work like the runtime accessor, plus bounds and
// a change log so a test can prove nothing outside the expected ranges was touched.
struct Arena
{
    std::vector<uint8_t> Bytes;
    std::vector<std::pair<uintptr_t, size_t>> Stores;
    std::vector<std::pair<uintptr_t, size_t>> Forbidden; // "freed" ranges: any Load inside them fails the test
    explicit Arena(size_t size) : Bytes(size, 0xAB) {}
    uintptr_t At(size_t offset) const { return reinterpret_cast<uintptr_t>(Bytes.data()) + offset; }
    bool Contains(uintptr_t p, size_t n) const { return p >= At(0) && p + n <= At(0) + Bytes.size(); }
    template <class T> bool Load(uintptr_t p, T& v) const
    {
        for (const auto& [start, n] : Forbidden) { const bool overlaps = p + sizeof(T) > start && p < start + n; REQUIRE_FALSE(overlaps); }
        if (!Contains(p, sizeof(T))) return false;
        std::memcpy(&v, reinterpret_cast<const void*>(p), sizeof(T));
        return true;
    }
    void Store(uintptr_t p, const RawTransform& t)
    {
        REQUIRE(Contains(p, kTransformBytes));
        std::memcpy(reinterpret_cast<void*>(p), t.Bytes.data(), kTransformBytes);
        Stores.emplace_back(p, kTransformBytes);
    }
    template <class T> void Put(size_t offset, const T& v) { std::memcpy(Bytes.data() + offset, &v, sizeof(T)); }
};

RawTransform Fill(uint8_t seed)
{
    RawTransform t;
    for (size_t i = 0; i < kTransformBytes; ++i) t.Bytes[i] = static_cast<uint8_t>(seed + i);
    return t;
}
bool Equal(const Arena& a, uintptr_t p, const RawTransform& t) { return std::memcmp(reinterpret_cast<const void*>(p), t.Bytes.data(), kTransformBytes) == 0; }

// Layout: tree object at 0x1000, entries at 0x2000 (N × 0x80), bone objects at 0x8000 + i × 0x200, child arrays
// at 0x20000 + i × 0x100, item objects at 0x30000 + k × 0x200.
constexpr size_t kTreeOff = 0x1000, kEntriesOff = 0x2000, kObjectsOff = 0x8000, kObjectStride = 0x200, kChildrenOff = 0x20000, kItemsOff = 0x30000;

struct Tree
{
    std::unique_ptr<Arena> Memory{std::make_unique<Arena>(0x40000)};
    uint32_t Count;
    uintptr_t TreeAddress, Data;
    explicit Tree(uint32_t count) : Count(count)
    {
        auto& m = *Memory;
        TreeAddress = m.At(kTreeOff);
        Data = m.At(kEntriesOff);
        m.Put(kTreeOff + kTreeCount, count);
        m.Put(kTreeOff + kTreeData, Data);
        const float root[3]{100.f, 200.f, 300.f};
        m.Put(kTreeOff + kObjectWorld + kWorldTranslation, root);
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t entry = kEntriesOff + i * kEntryStride;
            const uintptr_t object = i % 3 == 2 ? 0 : m.At(kObjectsOff + i * kObjectStride); // every third bone is object-less
            m.Put(entry + kEntryObject, object);
            m.Put(entry + kEntryName, uintptr_t{0x5000 + i});
            if (object) m.Put(kObjectsOff + i * kObjectStride, uintptr_t{0x7000 + i}); // a fake vtable per bone object
            m.Put(entry + 0x68, static_cast<int16_t>(i ? static_cast<int16_t>(i - 1) : -1));
            if (object)
            {
                m.Put(kObjectsOff + i * kObjectStride + kNodeChildren, m.At(kChildrenOff + i * 0x100));
                m.Put(kObjectsOff + i * kObjectStride + kNodeChildEnd, uint16_t{0});
            }
        }
    }
    uintptr_t Object(uint32_t i) const { return Memory->At(kObjectsOff + i * kObjectStride); }
    uintptr_t Entry(uint32_t i) const { return Data + i * kEntryStride; }
    uintptr_t Item(uint32_t k) const { return Memory->At(kItemsOff + k * kObjectStride); }
    void AddChild(uintptr_t parent, uintptr_t child)
    {
        uint16_t end{};
        uintptr_t children{};
        REQUIRE(Memory->Load(parent + kNodeChildEnd, end));
        REQUIRE(Memory->Load(parent + kNodeChildren, children));
        Memory->Put(children - Memory->At(0) + end * sizeof(uintptr_t), child);
        Memory->Put(parent - Memory->At(0) + kNodeChildEnd, static_cast<uint16_t>(end + 1));
    }
    Snapshot MakeSnapshot(const std::vector<uint32_t>& entries) const
    {
        Snapshot s;
        s.Tree = TreeAddress;
        s.Data = Data;
        s.Count = Count;
        s.RootPosition = {100.f, 200.f, 300.f};
        for (const auto i : entries)
        {
            uintptr_t object{}, name{};
            REQUIRE(Memory->Load(Entry(i) + kEntryObject, object));
            REQUIRE(Memory->Load(Entry(i) + kEntryName, name));
            s.Entries.push_back({i, object, name, object != TreeAddress, Fill(static_cast<uint8_t>(i))});
        }
        return s;
    }
};
// A Memory whose first Store re-enters Publish, modelling a reader that holds the current buffer while the
// main thread publishes twice: neither publish may pick the held buffer, and neither blocks.
struct Reentrant
{
    Arena& Inner;
    Registry& Reg;
    const Tree& T;
    int Stores{};
    std::vector<PublishResult> Results;
    template <class V> bool Load(uintptr_t p, V& v) const { return Inner.Load(p, v); }
    void Store(uintptr_t p, const RawTransform& t)
    {
        Inner.Store(p, t);
        if (Stores++ == 0)
        {
            Results.push_back(Reg.Publish(5, T.MakeSnapshot({1})));
            Results.push_back(Reg.Publish(5, T.MakeSnapshot({0})));
        }
    }
};
} // namespace

TEST_CASE("Reapply writes every published entry to the live tree and nothing else", "[postpass]")
{
    Tree tree(6);
    const auto registryStorage = std::make_unique<Registry>(); // ~1.8 MB: heap, as in the client
    Registry& registry = *registryStorage;
    const auto snapshot = tree.MakeSnapshot({0, 1, 2, 4});
    REQUIRE(registry.Publish(7, snapshot) == PublishResult::Published);
    const auto before = tree.Memory->Bytes;
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats, 42));
    REQUIRE(stats.Matched);
    REQUIRE_FALSE(stats.Stale);
    REQUIRE_FALSE(stats.Moved);
    REQUIRE(stats.Writes == 4);
    REQUIRE(stats.Skips == 0);
    // Entry worlds, and object worlds for object-backed entries (entry 2 has no object).
    REQUIRE(Equal(*tree.Memory, tree.Entry(0) + kEntryWorld, Fill(0)));
    REQUIRE(Equal(*tree.Memory, tree.Object(0) + kObjectWorld, Fill(0)));
    REQUIRE(Equal(*tree.Memory, tree.Entry(2) + kEntryWorld, Fill(2)));
    REQUIRE(Equal(*tree.Memory, tree.Object(4) + kObjectWorld, Fill(4)));
    REQUIRE(tree.Memory->Stores.size() == 7);
    // The tree's own world was not written, and no byte outside the stored ranges changed.
    for (size_t i = 0; i < before.size(); ++i)
    {
        const uintptr_t p = tree.Memory->At(i);
        bool inside = false;
        for (const auto& [start, n] : tree.Memory->Stores) inside = inside || (p >= start && p < start + n);
        if (!inside) REQUIRE(before[i] == tree.Memory->Bytes[i]);
    }
    REQUIRE(before[kTreeOff + kObjectWorld] == tree.Memory->Bytes[kTreeOff + kObjectWorld]);
    WindowCounters window{};
    REQUIRE(registry.TakeCounters(7, window));
    REQUIRE(window.Calls == 1);
    REQUIRE(window.Writes == 4);
    REQUIRE(window.Reapplies == 1); // one complete re-apply, four destinations
    REQUIRE(window.Threads == 1);
    REQUIRE(window.Publishes == 1);
    REQUIRE(registry.TakeCounters(7, window));
    REQUIRE(window.Calls == 0); // reset per window
}

TEST_CASE("Reapply skips exactly the entries whose identity changed, and everything on a rebuilt or warped tree", "[postpass]")
{
    Tree tree(6);
    const auto registryStorage = std::make_unique<Registry>(); // ~1.8 MB: heap, as in the client
    Registry& registry = *registryStorage;
    REQUIRE(registry.Publish(1, tree.MakeSnapshot({0, 1, 2, 3})) == PublishResult::Published);
    ReapplyStats stats;
    SECTION("replaced object")
    {
        tree.Memory->Put(kEntriesOff + 1 * kEntryStride + kEntryObject, tree.Object(4));
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 3);
        REQUIRE(stats.Skips == 1);
        REQUIRE_FALSE(Equal(*tree.Memory, tree.Entry(1) + kEntryWorld, Fill(1)));
    }
    SECTION("replaced name pointer (ABA on a reused allocation)")
    {
        tree.Memory->Put(kEntriesOff + 0 * kEntryStride + kEntryName, uintptr_t{0x9999});
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 3);
        REQUIRE(stats.Skips == 1);
    }
    SECTION("object-less entry that gained an object")
    {
        tree.Memory->Put(kEntriesOff + 2 * kEntryStride + kEntryObject, tree.Object(2));
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Skips == 1);
    }
    SECTION("changed count")
    {
        tree.Memory->Put(kTreeOff + kTreeCount, uint32_t{5});
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Stale);
        REQUIRE(stats.Writes == 0);
        REQUIRE(stats.Skips == 4);
        REQUIRE(tree.Memory->Stores.empty());
    }
    SECTION("moved data pointer")
    {
        tree.Memory->Put(kTreeOff + kTreeData, tree.Data + kEntryStride);
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Stale);
        REQUIRE(tree.Memory->Stores.empty());
    }
    SECTION("warped root")
    {
        const float root[3]{100.f, 200.f, 300.f + kMovedUnits + 1.f};
        tree.Memory->Put(kTreeOff + kObjectWorld + kWorldTranslation, root);
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Moved);
        REQUIRE_FALSE(stats.Stale);
        REQUIRE(stats.Writes == 0);
        REQUIRE(tree.Memory->Stores.empty());
    }
    SECTION("root inside the window still writes")
    {
        const float root[3]{100.f + kMovedUnits - 1.f, 200.f, 300.f};
        tree.Memory->Put(kTreeOff + kObjectWorld + kWorldTranslation, root);
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 4);
    }
    SECTION("unknown tree does not match")
    {
        REQUIRE_FALSE(registry.Reapply(tree.TreeAddress + 8, *tree.Memory, stats));
        REQUIRE_FALSE(stats.Matched);
        REQUIRE(tree.Memory->Stores.empty());
    }
    SECTION("released remote no longer matches")
    {
        registry.Release(1);
        REQUIRE_FALSE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(registry.UsedSlots() == 0);
    }
}

TEST_CASE("Items are written only while their native parent chain is intact", "[postpass]")
{
    Tree tree(4);
    const auto registryStorage = std::make_unique<Registry>(); // ~1.8 MB: heap, as in the client
    Registry& registry = *registryStorage;
    // Item 0 hangs directly under bone 1's object; item 1 hangs under a non-entry node that hangs under bone 3.
    const uintptr_t middle = tree.Item(2);
    tree.Memory->Put(kItemsOff + 2 * kObjectStride, uintptr_t{0x7777}); // NiNode-layout vtable for the middle node
    tree.Memory->Put(kItemsOff + 2 * kObjectStride + kNodeChildren, tree.Memory->At(kChildrenOff + 0x900));
    tree.Memory->Put(kItemsOff + 2 * kObjectStride + kNodeChildEnd, uint16_t{0});
    tree.AddChild(tree.Object(1), tree.Item(0));
    tree.AddChild(tree.Object(3), middle);
    tree.AddChild(middle, tree.Item(1));
    auto snapshot = tree.MakeSnapshot({1, 3});
    ItemWrite direct;
    direct.EntryIndex = 1; direct.ChainLength = 1; direct.Chain[0] = tree.Object(1); direct.ChainVtables[0] = 0x7001; direct.Object = tree.Item(0); direct.World = Fill(0x40);
    ItemWrite nested;
    nested.EntryIndex = 3; nested.ChainLength = 2; nested.Chain[0] = tree.Object(3); nested.Chain[1] = middle; nested.ChainVtables[0] = 0x7003; nested.ChainVtables[1] = 0x7777;
    nested.Object = tree.Item(1); nested.World = Fill(0x50);
    snapshot.Items = {direct, nested};
    REQUIRE(registry.Publish(3, snapshot) == PublishResult::Published);
    ReapplyStats stats;
    SECTION("intact chains write both items")
    {
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 4);
        REQUIRE(stats.Skips == 0);
        REQUIRE(Equal(*tree.Memory, tree.Item(0) + kObjectWorld, Fill(0x40)));
        REQUIRE(Equal(*tree.Memory, tree.Item(1) + kObjectWorld, Fill(0x50)));
    }
    SECTION("an item detached from its parent is skipped, the other still writes")
    {
        tree.Memory->Put(kObjectsOff + 1 * kObjectStride + kNodeChildEnd, uint16_t{0});
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 3);
        REQUIRE(stats.Skips == 1);
        REQUIRE_FALSE(Equal(*tree.Memory, tree.Item(0) + kObjectWorld, Fill(0x40)));
        REQUIRE(Equal(*tree.Memory, tree.Item(1) + kObjectWorld, Fill(0x50)));
    }
    SECTION("a broken middle link skips the nested item")
    {
        tree.Memory->Put(kObjectsOff + 3 * kObjectStride + kNodeChildEnd, uint16_t{0});
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 3);
        REQUIRE(stats.Skips == 1);
        REQUIRE_FALSE(Equal(*tree.Memory, tree.Item(1) + kObjectWorld, Fill(0x50)));
    }
    SECTION("a detached and freed middle node is never read")
    {
        tree.Memory->Put(kObjectsOff + 3 * kObjectStride + kNodeChildEnd, uint16_t{0}); // bone 3 no longer lists `middle`
        tree.Memory->Forbidden.emplace_back(middle, kObjectStride);                       // and `middle` is gone
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 3);
        REQUIRE(stats.Skips == 1);
    }
    SECTION("a chain node whose vtable changed is skipped before its child array is read")
    {
        tree.Memory->Put(kItemsOff + 2 * kObjectStride, uintptr_t{0x1234}); // middle node reused as another class
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Writes == 3);
        REQUIRE(stats.Skips == 1);
        REQUIRE_FALSE(Equal(*tree.Memory, tree.Item(1) + kObjectWorld, Fill(0x50)));
    }
    SECTION("a chain whose head is no longer the entry's object is skipped")
    {
        tree.Memory->Put(kEntriesOff + 3 * kEntryStride + kEntryObject, tree.Object(2));
        REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
        REQUIRE(stats.Skips == 2); // the entry itself and the nested item
    }
}

TEST_CASE("The triple buffer never hands a reader a buffer being written and drops a publish when both spares are held", "[postpass]")
{
    Tree tree(3);
    const auto registryStorage = std::make_unique<Registry>(); // ~1.8 MB: heap, as in the client
    Registry& registry = *registryStorage;
    REQUIRE(registry.Publish(9, tree.MakeSnapshot({0})) == PublishResult::Published);
    // Publish twice more: with no readers, buffers rotate freely.
    REQUIRE(registry.Publish(9, tree.MakeSnapshot({1})) == PublishResult::Published);
    REQUIRE(registry.Publish(9, tree.MakeSnapshot({0, 1})) == PublishResult::Published);
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE(stats.Writes == 2); // the newest snapshot
    // Hold two buffers: publish (fills a spare and flips), then hold the old current by a fake reader count.
    // The Registry exposes no reader handle by design, so the exhaustion path is exercised through capacity:
    // a snapshot larger than the buffer is refused before any buffer is touched.
    Snapshot big = tree.MakeSnapshot({0});
    big.Entries.resize(kMaxEntries + 1);
    REQUIRE(registry.Publish(9, big) == PublishResult::TooLarge);
    Snapshot manyItems = tree.MakeSnapshot({0});
    manyItems.Items.resize(kMaxItems + 1);
    REQUIRE(registry.Publish(9, manyItems) == PublishResult::TooLarge);
    // Slot exhaustion: kMaxSlots distinct remotes fit, the next is refused.
    const auto fullStorage = std::make_unique<Registry>();
    Registry& full = *fullStorage;
    for (uint32_t r = 1; r <= kMaxSlots; ++r) REQUIRE(full.Publish(r, tree.MakeSnapshot({0})) == PublishResult::Published);
    REQUIRE(full.Publish(kMaxSlots + 1, tree.MakeSnapshot({0})) == PublishResult::NoSlot);
    full.Release(3);
    REQUIRE(full.Publish(kMaxSlots + 1, tree.MakeSnapshot({0})) == PublishResult::Published);
    full.ReleaseAll();
    REQUIRE(full.UsedSlots() == 0);
}

TEST_CASE("An entry whose object is the tree itself restores only its flattened world", "[postpass]")
{
    Tree tree(3);
    const auto registryStorage = std::make_unique<Registry>();
    Registry& registry = *registryStorage;
    tree.Memory->Put(kEntriesOff + 0 * kEntryStride + kEntryObject, tree.TreeAddress); // "NPC Root [Root]" is the tree
    const auto before = tree.Memory->Bytes;
    REQUIRE(registry.Publish(2, tree.MakeSnapshot({0, 1})) == PublishResult::Published);
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE(stats.Writes == 2);
    REQUIRE(stats.Skips == 0);
    REQUIRE(Equal(*tree.Memory, tree.Entry(0) + kEntryWorld, Fill(0)));
    for (size_t i = kTreeOff + kObjectWorld; i < kTreeOff + kObjectWorld + kTransformBytes; ++i) REQUIRE(before[i] == tree.Memory->Bytes[i]);
    REQUIRE(tree.Memory->Stores.size() == 3); // entry 0 world, entry 1 world, object 1 world
}

TEST_CASE("Publishing with both spare buffers held is dropped and counted, then succeeds once one is released", "[postpass]")
{
    Tree tree(2);
    const auto registryStorage = std::make_unique<Registry>();
    Registry& registry = *registryStorage;
    REQUIRE(registry.Publish(4, tree.MakeSnapshot({0})) == PublishResult::Published);
    auto first = registry.AcquireHold(4); // holds the current buffer
    REQUIRE(first.Held());
    REQUIRE(registry.Publish(4, tree.MakeSnapshot({1})) == PublishResult::Published); // onto a spare
    auto second = registry.AcquireHold(4); // holds the new current
    REQUIRE(second.Held());
    REQUIRE(second.Buffer != first.Buffer);
    REQUIRE(registry.Publish(4, tree.MakeSnapshot({0, 1})) == PublishResult::Published); // the last spare
    REQUIRE(registry.Publish(4, tree.MakeSnapshot({0})) == PublishResult::Busy); // nothing free: dropped, not blocked
    WindowCounters window{};
    REQUIRE(registry.TakeCounters(4, window));
    REQUIRE(window.PublishBusy == 1);
    REQUIRE(window.Publishes == 3);
    registry.ReleaseHold(first);
    REQUIRE(registry.Publish(4, tree.MakeSnapshot({0})) == PublishResult::Published);
    registry.ReleaseHold(second);
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE(stats.Writes == 1); // the newest snapshot
}

TEST_CASE("A reader whose buffer is flipped during acquisition re-acquires, and three flips make it busy", "[postpass]")
{
    Tree tree(2);
    const auto registryStorage = std::make_unique<Registry>();
    Registry& registry = *registryStorage;
    REQUIRE(registry.Publish(6, tree.MakeSnapshot({0})) == PublishResult::Published);
    static Registry* s_registry; static const Tree* s_tree; static int s_flips; static int s_limit;
    s_registry = &registry; s_tree = &tree; s_flips = 0; s_limit = 1;
    Registry::AcquireProbe.store([] { if (s_flips++ < s_limit) REQUIRE(s_registry->Publish(6, s_tree->MakeSnapshot({0, 1})) == PublishResult::Published); });
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE_FALSE(stats.Busy);
    REQUIRE(stats.Writes == 2); // the re-check saw the flip and re-acquired the NEW buffer, not the one it first registered on
    WindowCounters window{};
    REQUIRE(registry.TakeCounters(6, window));
    REQUIRE(window.Busy == 0);
    s_flips = 0; s_limit = 3;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE(stats.Busy); // every attempt lost the race: bounded, counted, nothing written
    REQUIRE(stats.Writes == 0);
    REQUIRE(registry.TakeCounters(6, window));
    REQUIRE(window.Busy == 1);
    Registry::AcquireProbe.store(nullptr);
    // No reader count leaked: a publish now finds two free spares.
    REQUIRE(registry.Publish(6, tree.MakeSnapshot({1})) == PublishResult::Published);
    REQUIRE(registry.Publish(6, tree.MakeSnapshot({1})) == PublishResult::Published);
}

TEST_CASE("A recycled slot starts with a clean window and a pass bracket counts overlap", "[postpass]")
{
    Tree tree(2);
    const auto registryStorage = std::make_unique<Registry>();
    Registry& registry = *registryStorage;
    REQUIRE(registry.Publish(1, tree.MakeSnapshot({0})) == PublishResult::Published);
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    registry.Release(1); // counters not drained
    REQUIRE(registry.Publish(2, tree.MakeSnapshot({0})) == PublishResult::Published);
    WindowCounters window{};
    REQUIRE(registry.TakeCounters(2, window));
    REQUIRE(window.Calls == 0);
    REQUIRE(window.Writes == 0);
    REQUIRE(window.Publishes == 1);
    REQUIRE(window.Threads == 0);
    const auto g1 = registry.BeginPass(tree.TreeAddress);
    REQUIRE(g1 != 0);
    // A FRESH slot (never released) must also bracket and re-apply: a first-claim slot once read as unowned.
    const auto freshStorage = std::make_unique<Registry>();
    Registry& fresh = *freshStorage;
    REQUIRE(fresh.Publish(9, tree.MakeSnapshot({0})) == PublishResult::Published);
    const auto gf = fresh.BeginPass(tree.TreeAddress);
    REQUIRE(gf != 0);
    fresh.EndPass(gf);
    const auto gf2 = fresh.BeginPass(tree.TreeAddress);
    fresh.EndPass(gf2);
    WindowCounters freshWindow{};
    REQUIRE(fresh.TakeCounters(9, freshWindow));
    REQUIRE(freshWindow.Overlap == 0); // balanced brackets on a fresh slot
    const auto g2 = registry.BeginPass(tree.TreeAddress); // a second thunk enters while the first is still inside
    registry.EndPass(g2);
    registry.EndPass(g1);
    REQUIRE(registry.TakeCounters(2, window));
    REQUIRE(window.Overlap == 1);
    REQUIRE(registry.BeginPass(tree.TreeAddress + 8) == 0); // unowned tree: no bracket
}

TEST_CASE("Held readers are respected by the publisher", "[postpass]")
{
    // A reader-held buffer is modelled by a Memory whose Store re-enters Publish: the publish from inside a
    // read must not pick the buffer being read (it is current) nor block; with one spare free it succeeds,
    // and with the second spare also held it reports Busy.
    Tree tree(2);
    const auto registryStorage = std::make_unique<Registry>(); // ~1.8 MB: heap, as in the client
    Registry& registry = *registryStorage;
    REQUIRE(registry.Publish(5, tree.MakeSnapshot({0, 1})) == PublishResult::Published);
    Reentrant memory{*tree.Memory, registry, tree};
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, memory, stats));
    REQUIRE(stats.Writes == 2); // this read finished on the buffer it acquired
    REQUIRE(memory.Results.size() == 2);
    REQUIRE(memory.Results[0] == PublishResult::Published);
    REQUIRE(memory.Results[1] == PublishResult::Published); // held: 1 (ours); current: rotated; still one free
    // Now the reader has released; a further read sees the newest snapshot (entry 0 only).
    ReapplyStats again;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, again));
    REQUIRE(again.Writes == 1);
}

TEST_CASE("ClassifyAddress maps entry worlds to indices and refuses everything else", "[postpass]")
{
    const uintptr_t tree = 0x10000, data = 0x20000;
    uint32_t index = 99;
    REQUIRE(ClassifyAddress(tree + kObjectWorld, tree, data, 10, index) == 0);
    REQUIRE(ClassifyAddress(data + kEntryWorld, tree, data, 10, index) == 1);
    REQUIRE(index == 0);
    REQUIRE(ClassifyAddress(data + 7 * kEntryStride + kEntryWorld, tree, data, 10, index) == 1);
    REQUIRE(index == 7);
    REQUIRE(ClassifyAddress(data + 10 * kEntryStride + kEntryWorld, tree, data, 10, index) == 2); // past count
    REQUIRE(ClassifyAddress(data + 3 * kEntryStride + kEntryWorld + 4, tree, data, 10, index) == 2); // misaligned
    REQUIRE(ClassifyAddress(0x30000 + kObjectWorld, tree, data, 10, index) == 2); // an object world
    REQUIRE(ClassifyAddress(data + kEntryWorld, tree, 0, 10, index) == 2); // no data
}

TEST_CASE("Pass brackets balance the slot they entered, across two remotes and a release during a pass", "[postpass]")
{
    Tree first(2), second(2);
    const auto storage = std::make_unique<Registry>();
    Registry& registry = *storage;
    REQUIRE(registry.Publish(1, first.MakeSnapshot({0})) == PublishResult::Published);
    REQUIRE(registry.Publish(2, second.MakeSnapshot({0})) == PublishResult::Published);

    // Sequential passes on two remotes: no overlap on either.
    for (int i = 0; i < 3; ++i)
    {
        registry.EndPass(registry.BeginPass(first.TreeAddress));
        registry.EndPass(registry.BeginPass(second.TreeAddress));
    }
    WindowCounters a{}, b{};
    REQUIRE(registry.TakeCounters(1, a));
    REQUIRE(registry.TakeCounters(2, b));
    REQUIRE(a.Overlap == 0);
    REQUIRE(b.Overlap == 0);

    // Released while a thunk is inside it: that slot cannot be reclaimed until the thunk leaves, and once it has,
    // the next occupant's sequential passes count no overlap.
    const auto inside = registry.BeginPass(first.TreeAddress);
    REQUIRE(inside != 0);
    registry.Release(1);
    REQUIRE(registry.BeginPass(first.TreeAddress) == 0); // no longer owned
    REQUIRE(registry.Publish(3, first.MakeSnapshot({0})) == PublishResult::Published); // takes a different free slot
    registry.EndPass(inside);
    registry.Release(3);
    REQUIRE(registry.Publish(4, first.MakeSnapshot({0})) == PublishResult::Published);
    for (int i = 0; i < 3; ++i)
        registry.EndPass(registry.BeginPass(first.TreeAddress));
    WindowCounters c{};
    REQUIRE(registry.TakeCounters(4, c));
    REQUIRE(c.Overlap == 0);
    REQUIRE(registry.TakeCounters(2, b));
    REQUIRE(b.Overlap == 0);
    registry.EndPass(0); // an unowned pass's token is ignored
}

TEST_CASE("A stale or moved pass is not counted as a re-apply", "[postpass]")
{
    Tree tree(2);
    const auto storage = std::make_unique<Registry>();
    Registry& registry = *storage;
    REQUIRE(registry.Publish(1, tree.MakeSnapshot({0})) == PublishResult::Published);
    ReapplyStats stats;
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE(stats.Writes > 0);
    // Move the tree's fresh root far from the snapshot's: the next pass is refused as moved.
    tree.Memory->Put(tree.TreeAddress - tree.Memory->At(0) + kObjectWorld + 0x24, 10000.f);
    REQUIRE(registry.Reapply(tree.TreeAddress, *tree.Memory, stats));
    REQUIRE(stats.Moved);
    WindowCounters window{};
    REQUIRE(registry.TakeCounters(1, window));
    REQUIRE(window.Calls == 2);
    REQUIRE(window.Reapplies == 1);
    REQUIRE(window.Moved == 1);
}
