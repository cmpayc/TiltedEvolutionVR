#pragma once

#include <NetImmerse/NiObjectNET.h>

struct BSFixedString;

// SkyrimVR's NiAVObject carries 0x28 bytes more than SE's. Measured from NiNode's constructor,
// which initialises `children` at exactly sizeof(NiAVObject) and is otherwise byte identical in
// the two builds: SE 0x140D1C9CD `lea rax,[r14+0x110]` against VR 0x140C9C7AA
// `lea rax,[rdi+0x138]`, each followed by the same `mov [rax+0x10],0` /
// `mov dword [rax+0x14],0x10000` / `mov [rax+8],0` and the same NiTObjectArray vtable store.
// Corroborated by BSGeometry, whose NiBound also starts at 0x110 on SE and 0x138 on VR.
#if TP_SKYRIMVR
constexpr size_t kNiAVObjectSize = 0x138;
#else
constexpr size_t kNiAVObjectSize = 0x110;
#endif

struct NiAVObject : NiObjectNET
{
    virtual ~NiAVObject();

    virtual void sub_26();
    virtual void sub_27();
    virtual void sub_28();
    virtual void sub_29();
    // VR inserts one extra virtual somewhere between slot 0xA and this one, so on VR GetByName is
    // slot 0x2B rather than 0x2A: the recursive name search (id 76207, byte identical otherwise)
    // dispatches it as `call [rax+0x150]` on SE and `call [rax+0x158]` on VR. Nothing in the client
    // calls it, so where exactly the extra slot sits has not been pinned. Pin it before using this.
    virtual NiAVObject* GetByName(BSFixedString& aName);

    uint8_t pad30[kNiAVObjectSize - 0x30];
};

static_assert(sizeof(NiAVObject) == kNiAVObjectSize);
