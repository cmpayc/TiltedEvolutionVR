#pragma once

#include <Structs/BodyReferenceSkeleton.h>

struct Actor;
struct NiNode;

namespace BodyTracking
{
// Read-only VR adapter. Caller must hold a live actor/3D in a proven engine
// phase; VirtualQuery is a bounds check, not protection against destruction.
struct ReferenceDiagnostic
{
    uintptr_t StoredHolder{}, StoredRoot{}, ActorBase{}, HolderInterface{}, ExpectedRoot{};
    uint32_t GraphCount{}, ActiveGraph{};
    int SelectedGraph{-1};
    struct Graph { ReferenceGraphIdentity Identity; std::string RootName; };
    std::vector<Graph> Graphs;
};
// aReuse validates graph/root/array identities before keeping an already copied
// snapshot. Full arrays/names are copied only on content change. Diagnostics
// contain values, for logging outside the manager lock once per generation.
ReferenceFailure ReadBodyReferenceSkeleton(Actor& aActor, NiNode* apExpectedRoot,
                                          uint64_t aGeneration, ReferenceSkeleton& aOutput,
                                          ReferenceDiagnostic* apDiagnostic = nullptr, bool aReuse = false);
}
