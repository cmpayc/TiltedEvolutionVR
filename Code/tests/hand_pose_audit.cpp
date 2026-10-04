#include <catch2/catch.hpp>
#include "../client/Services/HandPoseAudit.h"
#include <array>
#include <vector>

namespace
{
struct Node
{
    int Id{};
    bool Intact{true};
};
struct Chain
{
    Node UpperArm, Forearm, Hand;
    std::vector<Node> UpperSubtree, ForeSubtree, HandSubtree;
};
auto Chains()
{
    return std::array<Chain, 2>{{
        {{1}, {2}, {3}, {{2}, {3}, {4}}, {{3}, {4}}, {{4}}},
        {{5}, {6}, {7}, {{6}, {7}, {8}}, {{7}, {8}}, {{8}}}
    }};
}
}

TEST_CASE("Head-only audit preserves both standing and history shoulder checks", "[hand-audit]")
{
    auto chains = Chains();
    // An inaccessible/repurposed unused descendant must never be read by this scope.
    chains[0].HandSubtree[0].Intact = false;
    std::vector<int> visited;
    auto check = [&](Node& node) { visited.push_back(node.Id); return node.Intact; };
    auto result = HandPoseAudit::AuditArms(chains, false, false, check);
    REQUIRE(visited == std::vector<int>{1, 5});
    REQUIRE(result.Checked == 2);
    REQUIRE(result.Skipped == 16);
    REQUIRE(result.Stale == 0);

    // Either shoulder can feed history/refusal/rest capture, even with no hands.
    for (auto& chain : chains) chain.UpperArm.Intact = false;
    visited.clear();
    result = HandPoseAudit::AuditArms(chains, false, false, check);
    REQUIRE(result.Stale == 2);
    REQUIRE(visited == std::vector<int>{1, 5});
}

TEST_CASE("Returning hands audits descendants skipped during head-only fallback", "[hand-audit]")
{
    auto chains = Chains();
    chains[1].Forearm.Intact = false;
    chains[0].HandSubtree[0].Intact = false;
    const auto check = [](Node& node) { return node.Intact; };
    REQUIRE(HandPoseAudit::AuditArms(chains, false, false, check).Stale == 0);
    const auto awake = HandPoseAudit::AuditArms(chains, false, true, check);
    REQUIRE(awake.Stale == 2);
    REQUIRE(awake.Checked == 18);
    REQUIRE(awake.Skipped == 0);
}

TEST_CASE("Cold identity capture is full even when the first packet is head-only", "[hand-audit]")
{
    auto chains = Chains();
    std::vector<int> visited;
    const auto result = HandPoseAudit::AuditArms(chains, true, false, [&](Node& node) {
        visited.push_back(node.Id);
        return node.Intact;
    });
    // Preserve the original full traversal, including its intentional overlap.
    REQUIRE(visited == std::vector<int>{2, 3, 4, 3, 4, 4, 1, 2, 3, 6, 7, 8, 7, 8, 8, 5, 6, 7});
    REQUIRE(result.Checked == 18);
    REQUIRE(result.Stale == 0);
    REQUIRE(result.Skipped == 0);
}
