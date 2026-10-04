#pragma once

#include <cstddef>
#include <initializer_list>

namespace HandPoseAudit
{
struct ArmResult
{
    std::size_t Stale{}, Checked{}, Skipped{};
};

// Traversal only: the caller still performs every current permission/identity
// check. No pointer, permission or validity answer is cached here.
template<class Chains, class Audit>
ArmResult AuditArms(Chains& chains, bool capture, bool hasHands, Audit&& audit) noexcept
{
    ArmResult result;
    const bool full = capture || hasHands;
    const auto check = [&](auto& node) {
        ++result.Checked;
        if (!audit(node)) ++result.Stale;
    };
    for (auto& chain : chains)
    {
        if (full)
        {
            for (auto* list : {&chain.UpperSubtree, &chain.ForeSubtree, &chain.HandSubtree})
                for (auto& node : *list) check(node);
        }
        else
            result.Skipped += chain.UpperSubtree.size() + chain.ForeSubtree.size() + chain.HandSubtree.size() + 2;

        // Head-only history, refusal diagnostics and standing head-rest capture
        // still read both shoulders. Keep their original checks on every pass.
        check(chain.UpperArm);
        if (full)
        {
            check(chain.Forearm);
            check(chain.Hand);
        }
    }
    return result;
}
}
