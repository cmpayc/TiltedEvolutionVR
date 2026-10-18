// What the pose sync puts on the wire outside the body stream: the server's bUseLegacyHandPose inside
// ServerSettings, and the grip fields the hand pose messages carry.
#include <catch2/catch.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Messages/AuthenticationResponse.h>
#include <Messages/NotifySettingsChange.h>
#include <Messages/RequestHandPose.h>
#include <Messages/NotifyHandPose.h>

using TiltedPhoques::Buffer;

namespace
{
template <class T> T RoundTrip(const T& acSent)
{
    Buffer buffer(1000);
    Buffer::Writer writer(&buffer);
    acSent.Serialize(writer);
    Buffer::Reader reader(&buffer);
    uint64_t opcode{};
    reader.ReadBits(opcode, 8);
    T received{};
    received.DeserializeRaw(reader);
    return received;
}
}

TEST_CASE("ServerSettings carries bUseLegacyHandPose after every earlier field, both ways", "[pose-sync][settings]")
{
    for (const bool cLegacy : {false, true})
    {
        AuthenticationResponse sent{};
        sent.Type = AuthenticationResponse::ResponseType::kAccepted;
        sent.Settings.NotOwnedDeadBodyRagdoll = !cLegacy; // the field before it, set to the opposite value
        sent.Settings.BlockRemotePlayerActivation = cLegacy;
        sent.Settings.UseLegacyHandPose = cLegacy;
        const auto received = RoundTrip(sent);
        REQUIRE(received == sent);
        REQUIRE(received.Settings.UseLegacyHandPose == cLegacy);
        REQUIRE(received.Settings.NotOwnedDeadBodyRagdoll == !cLegacy);

        NotifySettingsChange change{};
        change.Settings = sent.Settings;
        REQUIRE(RoundTrip(change).Settings.UseLegacyHandPose == cLegacy);
    }

    ServerSettings a{}, b{};
    REQUIRE_FALSE(a.UseLegacyHandPose); // default: body tracking
    b.UseLegacyHandPose = true;
    REQUIRE(a != b);
}

TEST_CASE("Hand pose messages carry both grips, per hand, with their validity", "[pose-sync][hand]")
{
    RequestHandPose request{};
    request.Id = 0x1234;
    request.HandsActive = true;
    request.EyeHeight = 121.5f;
    request.LeftGripRotation = glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f));
    request.RightGripRotation = glm::normalize(glm::quat(0.2f, 0.7f, 0.1f, -0.6f));
    request.LeftGripOffset = {1.25f, -2.5f, 3.75f};
    request.RightGripOffset = {-4.5f, 0.125f, 9.f};
    request.LeftGripValid = true;
    request.RightGripValid = false;

    const auto received = RoundTrip(request);
    REQUIRE(received.Id == request.Id);
    REQUIRE(received.LeftGripOffset == request.LeftGripOffset);
    REQUIRE(received.RightGripOffset == request.RightGripOffset);
    REQUIRE(received.LeftGripValid);
    REQUIRE_FALSE(received.RightGripValid);
    REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(received.LeftGripRotation), static_cast<glm::quat>(request.LeftGripRotation))) > 0.999f);
    REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(received.RightGripRotation), static_cast<glm::quat>(request.RightGripRotation))) > 0.999f);
    REQUIRE(received.EyeHeight == request.EyeHeight);

    NotifyHandPose notify{};
    notify.Id = 0x77;
    notify.LeftGripRotation = request.LeftGripRotation;
    notify.RightGripRotation = request.RightGripRotation;
    notify.LeftGripOffset = request.LeftGripOffset;
    notify.RightGripOffset = request.RightGripOffset;
    notify.LeftGripValid = false;
    notify.RightGripValid = true;
    const auto relayed = RoundTrip(notify);
    REQUIRE(relayed.Id == notify.Id);
    REQUIRE(relayed.LeftGripOffset == notify.LeftGripOffset);
    REQUIRE(relayed.RightGripOffset == notify.RightGripOffset);
    REQUIRE_FALSE(relayed.LeftGripValid);
    REQUIRE(relayed.RightGripValid);
    REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(relayed.RightGripRotation), static_cast<glm::quat>(notify.RightGripRotation))) > 0.999f);
}

TEST_CASE("Hand pose messages carry the sender's drawn state, both ways and both values", "[pose-sync][hand]")
{
    for (const bool drawn : {true, false})
    {
        RequestHandPose request{};
        request.Id = 0x1234;
        request.Drawn = drawn;
        request.RightGripValid = true; // the field before it, so a misordered read shows
        const auto received = RoundTrip(request);
        REQUIRE(received.Drawn == drawn);
        REQUIRE(received.RightGripValid);
        REQUIRE(received == request);

        NotifyHandPose notify{};
        notify.Id = 0x77;
        notify.Drawn = drawn;
        notify.RightGripValid = !drawn;
        const auto relayed = RoundTrip(notify);
        REQUIRE(relayed.Drawn == drawn);
        REQUIRE(relayed.RightGripValid == !drawn);
        REQUIRE(relayed == notify);
    }
}
