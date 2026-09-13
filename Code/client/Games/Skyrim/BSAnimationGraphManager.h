#pragma once

#include <Games/Primitives.h>

#include <Havok/BShkbAnimationGraph.h>

struct BSFixedString;

struct BSAnimationGraphManager
{
    virtual ~BSAnimationGraphManager();
    virtual void sub_1(void* apUnk1);

    void Release()
    {
        if (InterlockedDecrement(&refCount) == 0)
            this->~BSAnimationGraphManager();
    }

    volatile LONG refCount;
    void* pad_ptrs[6];
    BSTSmallArray<BShkbAnimationGraph> animationGraphs; // 40 - 20
    // Anniversary Edition added one pointer between the graph array and the lock; SkyrimVR keeps
    // the older layout, so the lock and everything after it sit 8 bytes lower. The constructor
    // states it, and the two builds are byte identical there apart from these displacements:
    //   SE 0x140BA3653  lea rdx,[rdi+0xA0] / mov [rdi+0xA0],rsi / mov [rdi+0xA8],rsi / mov [rdi+0xB0],esi
    //   VR 0x140B1C533  lea rdx,[rdi+0x98] / mov [rdi+0x98],rsi / mov [rdi+0xA0],rsi / mov [rdi+0xA8],esi
    // The trailing comments here were always the older column and they agree, which is what VR is
    // built from. `animationGraphs` stays at 0x40: the same constructor writes
    // `mov dword [this+0x40],0x80000000` in both, and the game's own graph walk reads capacity at
    // 0x40, data at 0x48 and size at 0x50 identically in both builds.
#if TP_SKYRIMVR
    void* pad_ptrs2[8];
#else
    void* pad_ptrs2[9];
#endif
    BSRecursiveLock lock;  // 98 - 4C
    void* unkPtrAfterLock; // A0 - 58

#if TP_PLATFORM_32
    void* unkPtrOldrim;
#endif

    uint32_t animationGraphIndex; // A8 - 5C

    SortedMap<uint32_t, String> DumpAnimationVariables(bool aPrintVariables);
    uint64_t GetDescriptorKey(int aForceIndex = -1);
    uint32_t ReSendEvent(BSFixedString* apEventName);
};

#if TP_SKYRIMVR
static_assert(offsetof(BSAnimationGraphManager, animationGraphs) == 0x40);
static_assert(offsetof(BSAnimationGraphManager, lock) == 0x98);
static_assert(offsetof(BSAnimationGraphManager, animationGraphIndex) == 0xA8);
#elif TP_PLATFORM_64
static_assert(offsetof(BSAnimationGraphManager, animationGraphs) == 0x40);
static_assert(offsetof(BSAnimationGraphManager, lock) == 0xA0);
static_assert(offsetof(BSAnimationGraphManager, animationGraphIndex) == 0xB0);
#else
static_assert(offsetof(BSAnimationGraphManager, animationGraphs) == 0x20);
static_assert(offsetof(BSAnimationGraphManager, lock) == 0x4C);
static_assert(offsetof(BSAnimationGraphManager, animationGraphIndex) == 0x5C);
#endif
