#pragma once

#include <NetImmerse/NiPointer.h>
#include <NetImmerse/NiAVObject.h>
#include <NetImmerse/NiProperty.h>

// The head geometry the facegen path works on is a BSGeometry, and its constructor states the
// layout of both builds outright. SE 0x140D37A70 writes the NiBound floats to [this+0x110],
// +0x114, +0x118 and +0x11C, then `lea rcx,[rbx+0x120]` with element size 8 and count 2 for the
// property pair. VR 0x140CB7450 is the same function with the bound at 0x138, six more floats at
// 0x148 through 0x160 that SE does not have, and then `lea rcx,[rbx+0x160]` with the same size and
// count. So the pair the client reads sits at 0x120/0x128 on SE and 0x160/0x168 on VR.
//
// The game itself agrees where it does exactly what FaceGenSystem::Update does: id 40699,
// SE 0x14074A2D0 and VR 0x1406D85F0, byte identical apart from displacements, calls
// CastToNiTriBasedGeom (vtable slot 9 in both) and then reads `mov rbx,[rax+0x128]` on SE against
// `mov rbx,[rax+0x168]` on VR before comparing the RTTI and taking `[prop+0x78]` for the material.
//
// unkProperty1 and unkProperty2 are the NiBound, not properties. The names are left alone; what
// matters is that `effect` lands on the second entry of the property pair in both builds.
#if TP_SKYRIMVR
constexpr size_t kNiGeometryEffectOffset = 0x168;
#else
constexpr size_t kNiGeometryEffectOffset = 0x128;
#endif

struct NiGeometry : NiAVObject
{
    virtual ~NiGeometry();

    NiPointer<NiProperty> unkProperty1;
    NiPointer<NiProperty> unkProperty2;
#if TP_SKYRIMVR
    uint8_t padVRGeometry[0x18];
#endif
    uintptr_t unkB0;
    NiPointer<NiProperty> effect;
    uintptr_t unkB8;
};

static_assert(offsetof(NiGeometry, effect) == kNiGeometryEffectOffset);
