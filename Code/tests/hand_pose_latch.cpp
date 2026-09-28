// The ownership latch between the body stream and the legacy hand-pose path, the idle rule and the pose sync
// mode: pure functions, so every branch the render hook takes is pinned here.
#include <catch2/catch.hpp>
#include "../client/Services/HandPoseLatch.h"
#include "../client/Services/PoseSyncMode.h"

using namespace HandPoseLatch;

TEST_CASE("A body write owns the frame and holds the legacy path off for the latch window, then hands over", "[latch]")
{
    State s;
    REQUIRE(Decide(true, true, s, 1000) == Owner::Body);
    REQUIRE(s.OwnedAtMs == 1000);
    REQUIRE(Decide(false, true, s, 1000) == Owner::Latched);   // same frame
    REQUIRE(Decide(false, false, s, 1249) == Owner::Latched);  // one ms inside, stream gone
    REQUIRE(Decide(false, false, s, 1250) == Owner::Legacy);   // window closed
    REQUIRE(Decide(false, false, s, 5000) == Owner::Legacy);
    State never;
    REQUIRE(Decide(false, false, never, 1000) == Owner::Legacy); // never body-owned, not ready: today's behavior
    State behind; behind.OwnedAtMs = 2000;
    REQUIRE(Decide(false, false, behind, 1000) == Owner::Legacy); // clock behind the stamp never latches forever
    State window; window.OwnedAtMs = 1000;
    REQUIRE(Decide(false, false, window, 1100, 50) == Owner::Legacy);
    REQUIRE(kBodyLatchMs == 250);
}

TEST_CASE("A Ready stream that has not written holds the legacy path off once, for one window, then no longer", "[latch]")
{
    State s;
    REQUIRE(Decide(false, true, s, 1000) == Owner::Latched);   // first acquire: claim starts
    REQUIRE(s.ClaimedAtMs == 1000);
    REQUIRE(Decide(false, true, s, 1249) == Owner::Latched);
    REQUIRE(Decide(false, true, s, 1250) == Owner::Legacy);    // still Ready, still no write: legacy poses, as before this build
    REQUIRE(Decide(false, true, s, 9000) == Owner::Legacy);    // a claim never renews while Ready persists
    REQUIRE(s.ClaimedAtMs == 1000);
    REQUIRE(Decide(true, true, s, 9001) == Owner::Body);       // the first write clears the claim and owns
    REQUIRE(s.ClaimedAtMs == 0);
    REQUIRE(Decide(false, false, s, 9300) == Owner::Legacy);   // stream gone, window over
    REQUIRE(Decide(false, true, s, 9300) == Owner::Latched);   // a new Ready episode gets a new bounded claim
    REQUIRE(s.ClaimedAtMs == 9300);
    REQUIRE(Decide(false, false, s, 9400) == Owner::Legacy);   // not Ready clears the claim
    REQUIRE(s.ClaimedAtMs == 0);
}

TEST_CASE("Head-only idle is skipped whenever hands are absent in body mode, never in legacy mode", "[latch]")
{
    REQUIRE(HeadOnlyIdle(false, true));
    REQUIRE_FALSE(HeadOnlyIdle(true, true));
    REQUIRE_FALSE(HeadOnlyIdle(false, false)); // legacy: the head follows with no hands, drawn or not
    REQUIRE_FALSE(HeadOnlyIdle(true, false));
}

TEST_CASE("Pose sync mode follows the connection and the server's bUseLegacyHandPose", "[latch][mode]")
{
    using namespace PoseSyncMode;
    REQUIRE(Decide(false, false) == Mode::Offline);
    REQUIRE(Decide(false, true) == Mode::Offline);
    REQUIRE(Decide(true, false) == Mode::Body);
    REQUIRE(Decide(true, true) == Mode::Legacy);
    REQUIRE(BodyLane(Mode::Body));
    REQUIRE_FALSE(BodyLane(Mode::Legacy));
    REQUIRE_FALSE(BodyLane(Mode::Offline));
    REQUIRE(SyncHandsWhileDrawn(Mode::Body));
    REQUIRE_FALSE(SyncHandsWhileDrawn(Mode::Legacy));
    REQUIRE_FALSE(SyncHandsWhileDrawn(Mode::Offline));
}
