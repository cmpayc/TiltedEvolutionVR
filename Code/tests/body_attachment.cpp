#include <catch2/catch.hpp>
#include <Structs/BodyPoseAttachment.h>
#include <string>
#include <vector>

using namespace BodyTracking;
namespace
{
struct AttachmentNode { std::string Name; int Parent; };
std::vector<AttachmentNode> AttachmentTree()
{
    return {{"Root", -1}, {"RightWandNode", 0}, {"RightWeaponOffsetNode", 1},
        {"NPC R Hand [RHnd]", 0}, {"WEAPON", 3}, {"Weapon  (00013982)", 4},
        {"NPC L Hand [LHnd]", 0}, {"SHIELD", 6}, {"Shield  (00012EB6)", 7},
        {"WeaponBack", 0}, {"Weapon  (00013982)", 9},
        {"Separate HIGGS root", -1}, {"Weapon  (00013982)", 11}};
}
}

TEST_CASE("Body grip finds the observed hand-attached item with an empty wand and agrees with receiver identity", "[body][grip]")
{
    const auto nodes = AttachmentTree();
    const auto right = SelectAttachedItem(nodes, 0);
    REQUIRE(right.Index == 5);
    REQUIRE(right.Form == 0x13982);
    REQUIRE_FALSE(right.Ambiguous);
    REQUIRE_FALSE(right.InvalidTree);
    REQUIRE(SelectAttachedItem(nodes, 0, right.Form).Index == right.Index);
    const auto left = SelectAttachedItem(nodes, 1);
    REQUIRE(left.Index == 8);
    REQUIRE(left.Form == 0x12eb6);
    REQUIRE(SelectAttachedItem(nodes, 1, left.Form).Index == left.Index);
    REQUIRE(SelectAttachedItem(nodes, 1, right.Form).Index == -1);
    REQUIRE(SelectAttachedItem(nodes, 0, 0x1234).Index == -1);
}

TEST_CASE("Body grip refuses ambiguous and invalid trees and excludes wrong attachments", "[body][grip]")
{
    auto nodes = AttachmentTree();
    SECTION("ambiguous source with distinct forms; exact receiver remains unique")
    {
        nodes.push_back({"Weapon  (00012345)", 4});
        REQUIRE(SelectAttachedItem(nodes, 0).Ambiguous);
        REQUIRE_FALSE(SelectAttachedItem(nodes, 0, 0x13982).Ambiguous);
        REQUIRE(SelectAttachedItem(nodes, 0, 0x13982).Index == 5);
    }
    SECTION("same-form duplicates refuse both sides")
    {
        nodes.push_back({"Weapon  (00013982)", 4});
        REQUIRE(SelectAttachedItem(nodes, 0).Ambiguous);
        REQUIRE(SelectAttachedItem(nodes, 0, 0x13982).Ambiguous);
    }
    SECTION("proxy attachment is not WEAPON")
    {
        nodes[4].Name = "WEAPON Proxy";
        REQUIRE(SelectAttachedItem(nodes, 0).Index == -1);
    }
    SECTION("holster and detached HIGGS item do not replace missing drawn item")
    {
        nodes[5].Parent = 9;
        REQUIRE(SelectAttachedItem(nodes, 0).Index == -1);
    }
    SECTION("out of bounds parent")
    {
        nodes[5].Parent = 100;
        REQUIRE(SelectAttachedItem(nodes, 0).InvalidTree);
    }
    SECTION("ancestry cycle is bounded")
    {
        nodes[4] = {"Bad parent", 4};
        REQUIRE(SelectAttachedItem(nodes, 0).InvalidTree);
    }
    SECTION("invalid hand") { REQUIRE(SelectAttachedItem(nodes, 2).InvalidTree); }
}

TEST_CASE("Body item identity keeps the equipped-root format without a weapon ID list", "[body][grip]")
{
    REQUIRE(ItemForm("Weapon  (00013982)") == 0x13982);
    REQUIRE(ItemForm("Shield (aBcD1234)") == 0xabcd1234);
    REQUIRE(ItemForm("Torch (00123456)") == 0x123456);
    REQUIRE(ItemForm("Staff (00123456)") == 0x123456);
    REQUIRE(ItemForm("Bow (00123456)") == 0x123456);
    REQUIRE(ItemForm("Crossbow (00123456)") == 0x123456);
    REQUIRE(ItemForm("Weapon  (FFFFFFFF)") == UINT32_MAX);
    REQUIRE(ItemForm("Weapon (00000000)") == 0);
    REQUIRE(ItemForm("Weapon(00013982)") == 0);
    REQUIRE(ItemForm("Weapon (0013982)") == 0);
    REQUIRE(ItemForm("Weapon (0001398Z)") == 0);
    REQUIRE(ItemForm("Weapon (00013982) trailing") == 0);
}
