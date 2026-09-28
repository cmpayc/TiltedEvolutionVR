#include <catch2/catch.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/glm.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/BodyPoseHistory.h>
#include <Structs/BodyPoseComposition.h>
#include <Structs/BodyPoseRelay.h>
#include <Structs/BodyPoseSource.h>
#include <Structs/BodyReferenceSkeleton.h>
#include <limits>

using namespace BodyTracking;

TEST_CASE("Solo admission waits for actual load completion and bounds retries", "[body]")
{
    SoloPreflightSchedule gate;
    for (uint64_t now = 1000; now < 60000; now += 2000) REQUIRE_FALSE(gate.Attempt(now));
    REQUIRE(gate.ObserveLoad(60000));
    REQUIRE_FALSE(gate.Attempt(59999));
    for (uint64_t now = 60000; now < 80000; now += 2000)
    {
        REQUIRE(gate.Attempt(now));
        REQUIRE_FALSE(gate.Attempt(now+1));
    }
    REQUIRE(gate.Attempts == 10);
    REQUIRE_FALSE(gate.Attempt(100000));
    REQUIRE_FALSE(gate.ObserveLoad(60000));
    REQUIRE(gate.ObserveLoad(101000));
    REQUIRE(gate.Attempt(101000));
    REQUIRE(gate.Attempts == 1);
}

TEST_CASE("Reference graph selection requires one exact holder and root, independent of active index", "[body]")
{
    std::array<ReferenceGraphIdentity, 2> graphs{{{1, 100, 200, true}, {2, 100, 300, true}}};
    REQUIRE(SelectReferenceGraph(graphs, 100, 300) == 1);
    REQUIRE(SelectReferenceGraph(graphs, 100, 200) == 0);
    REQUIRE(SelectReferenceGraph(graphs, 101, 300) == -1);
    REQUIRE(SelectReferenceGraph(graphs, 100, 400) == -1);
    graphs[1].Root = 200;
    REQUIRE(SelectReferenceGraph(graphs, 100, 200) == -2);
    graphs[1].Readable = false;
    REQUIRE(SelectReferenceGraph(graphs, 100, 200) == 0);
}

TEST_CASE("Reference refusal reports the actual mismatching bone scales and parents", "[body]")
{
    ReferenceSkeleton reference;
    reference.Bones = {{"root", -1, {}, false}, {"hand", 0, {}, true}};
    std::vector<Joint> joints(2);
    joints[1].Parent = 0;
    std::array<std::string_view, 2> names{"root", "hand"};
    ReferenceBindDiagnostic why;
    joints[1].Local.Scale = .0001f;
    REQUIRE(BindReferenceLocals(joints, names, reference, &why) == ReferenceFailure::Transform);
    REQUIRE(why.Bone == "hand");
    REQUIRE(why.LiveScale == .0001f);
    REQUIRE(why.ReferenceScale == 1.f);
    joints[1].Local.Scale = 1.f;
    joints[1].Parent = 1;
    REQUIRE(BindReferenceLocals(joints, names, reference, &why) == ReferenceFailure::Topology);
    REQUIRE(why.LiveParent == "hand");
    REQUIRE(why.ReferenceParent == "root");
    joints[1].Parent = 0;
    REQUIRE(BindReferenceLocals(joints, names, reference, &why) == ReferenceFailure::None);
    REQUIRE(why.Bone.empty());
}

using namespace TiltedPhoques;

#include "body_reference_fixture.inl"

namespace
{
Pose MakePose(uint64_t aSequence = 1, uint64_t aTick = 1000)
{
    Pose pose{};
    pose.Sequence = aSequence;
    pose.CaptureTick = aTick;
    pose.Generation = 1;
    pose.Mask = kKnownMask;
    pose.GripMask = 3;
    pose.GripItems = {GameId{1, 0x1234}, GameId{2, 0x5678}};
    for (size_t i = 0; i < pose.Bones.size(); ++i)
    {
        pose.Bones[i].Position = glm::vec3(-15.375f + i, 0.0625f, 83.8125f);
        pose.Bones[i].Rotation = glm::angleAxis(0.3f, glm::vec3(0, 0, 1));
    }
    pose.Grips[0].Position = glm::vec3(0.03125f, -2.5f, 3.375f);
    return pose;
}
}

TEST_CASE("Body codec retains fractional positions within a single small packet", "[body]")
{
    auto source = MakePose(0x100000004ull, 0x100000009ull);
    REQUIRE(source.IsValid());
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    source.Serialize(writer);
    const auto bytes = writer.Size();
    REQUIRE(bytes == 682);
    Buffer exact(buffer.GetData(), bytes);
    Buffer::Reader reader(&exact);
    Pose copy{};
    copy.Deserialize(reader);
    REQUIRE(copy.IsValid());
    REQUIRE(copy.Sequence == source.Sequence);
    REQUIRE(copy.CaptureTick == source.CaptureTick);
    for (size_t i = 0; i < source.Bones.size(); ++i)
    {
        REQUIRE(copy.Bones[i].Position == source.Bones[i].Position);
        REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(copy.Bones[i].Rotation), static_cast<glm::quat>(source.Bones[i].Rotation))) > 0.99999f);
    }
    REQUIRE(copy.Grips[0].Position == source.Grips[0].Position);
    REQUIRE(copy.GripItems == source.GripItems);
    // Every truncated byte length, including halfway through a quaternion.
    for (size_t cut = 0; cut < bytes; ++cut)
    {
        Buffer truncated(buffer.GetData(), cut);
        Buffer::Reader shortReader(&truncated);
        Pose invalid{};
        invalid.Deserialize(shortReader);
        REQUIRE_FALSE(invalid.IsValid());
    }
}

TEST_CASE("Body rejects incomplete, unknown and non-finite data", "[body]")
{
    auto pose = MakePose();
    SECTION("missing foot") { pose.Mask &= ~(1u << 8); }
    SECTION("unknown slot") { pose.Mask |= 1u << 31; }
    SECTION("unknown grip") { pose.GripMask = 4; }
    SECTION("grip without exact item") { pose.GripItems[0] = {}; }
    SECTION("NaN") { pose.Bones[5].Position.x = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("infinity") { pose.Bones[17].Position.z = std::numeric_limits<float>::infinity(); }
    SECTION("out of bounds") { pose.Bones[25].Position.y = kMaxTranslation + 0.1f; }
    SECTION("zero quaternion") { pose.Bones[0].Rotation = glm::quat(0, 0, 0, 0); }
    REQUIRE_FALSE(pose.IsValid());
}

TEST_CASE("Body schema and explicit source loss survive the codec", "[body]")
{
    auto source = MakePose();
    source.Mask = 0;
    source.GripMask = 0;
    REQUIRE(source.IsValid());
    REQUIRE_FALSE(source.HasBody());
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    source.Serialize(writer);
    REQUIRE(writer.Size() == 26);
    Buffer::Reader reader(&buffer);
    Pose copy{};
    copy.Deserialize(reader);
    REQUIRE(copy.IsValid());
    REQUIRE_FALSE(copy.HasBody());
    buffer[0] = 1; // old schema cannot silently decode with changed frame semantics
    reader.Reset();
    copy.Deserialize(reader);
    REQUIRE_FALSE(copy.IsValid());
}

TEST_CASE("Body opcodes dispatch through both real message factories", "[body]")
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    RequestBodyPose request{};
    request.Id = 12345;
    request.Body = MakePose();
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto result = ClientMessageFactory{}.Extract(reader);
    REQUIRE(result);
    REQUIRE(result->GetOpcode() == kRequestBodyPose);
    auto copy = CastUnique<RequestBodyPose>(std::move(result));
    REQUIRE(copy->Id == request.Id);
    REQUIRE(copy->Body.IsValid());
    writer.Reset();
    NotifyBodyPose notify{};
    notify.Id = request.Id;
    notify.ServerTick = 0x100000007ull;
    notify.Body = request.Body;
    notify.Serialize(writer);
    REQUIRE(writer.Size() < 720);
    reader.Reset();
    auto notification = ServerMessageFactory{}.Extract(reader);
    REQUIRE(notification);
    REQUIRE(notification->GetOpcode() == kNotifyBodyPose);
    auto received = CastUnique<NotifyBodyPose>(std::move(notification));
    REQUIRE(received->Id == notify.Id);
    REQUIRE(received->ServerTick == notify.ServerTick);
    REQUIRE(received->Body.IsValid());
}

TEST_CASE("Body follows root presentation delay and expires on source time", "[body]")
{
    History history{};
    auto first = MakePose(1, 990);
    auto second = MakePose(2, 1090);
    first.Bones[1].Position.x = 10.f;
    second.Bones[1].Position.x = 30.f;
    REQUIRE(history.Push(first, 1000));
    REQUIRE(history.Push(second, 1100));
    Pose selected{};
    REQUIRE(history.Select(1299, selected) == Selection::Warming);
    REQUIRE(history.Select(1350, selected) == Selection::Ready);
    REQUIRE(selected.Bones[1].Position.x == Approx(20.f));
    REQUIRE(history.Select(2091, selected) == Selection::Stale);
    REQUIRE_FALSE(selected.HasBody());
    REQUIRE_FALSE(history.Push(first, 1200));
    auto loss = MakePose(3, 1200);
    loss.Mask = loss.GripMask = 0;
    REQUIRE(history.Push(loss, 1210));
    REQUIRE(history.Select(1510, selected) == Selection::SourceLost);
    REQUIRE_FALSE(history.Push(second, 1220));
    auto recovered = MakePose(4, 1300);
    REQUIRE(history.Push(recovered, 1310));
    REQUIRE(history.Select(1510, selected) == Selection::SourceLost);
    REQUIRE(history.Select(1610, selected) == Selection::Ready);
    recovered.Sequence = 5;
    recovered.Generation = 2;
    recovered.CaptureTick = 1400;
    REQUIRE(history.Push(recovered, 1410));
    REQUIRE(history.Count == 1);
    REQUIRE(history.Select(1610, selected) == Selection::Warming);
}

TEST_CASE("Body history is bounded and rejects reversed server time", "[body]")
{
    History history{};
    for (uint64_t i = 1; i <= 100; ++i)
        REQUIRE(history.Push(MakePose(i, 1000 + i * 33), 1010 + i * 33));
    REQUIRE(history.Count == History::kCapacity);
    REQUIRE(history.Samples[0].Body.Sequence == 69);
    REQUIRE_FALSE(history.Push(MakePose(101, 4400), 4000));
    Pose selected{};
    REQUIRE(history.Select(1000, selected) == Selection::ClockOutsideWindow);
}

TEST_CASE("Body transform conversion preserves fractional positions, rotation and scale", "[body]")
{
    const auto yaw = glm::angleAxis(glm::radians(90.f), glm::vec3(0, 0, 1));
    Transform sender{{1000.f, -400.f, 20.f}, yaw, 0.7f};
    Transform measured{{996.5f, -398.25f, 55.f}, yaw, 0.7f};
    const auto relative = RelativeTo(sender, measured);
    REQUIRE(relative.Position.x == Approx(2.5f).margin(0.0001));
    REQUIRE(relative.Position.y == Approx(5.f).margin(0.0001));
    REQUIRE(relative.Position.z == Approx(50.f));
    const auto roundtrip = Compose(sender, relative);
    REQUIRE(glm::length(roundtrip.Position - measured.Position) < 0.0001f);
    Transform receiver{{2000.f, -800.f, 40.f}, glm::quat(1, 0, 0, 0), 1.03f};
    const auto placed = Compose(receiver, relative);
    REQUIRE(placed.Position.x == Approx(2002.575f));
    REQUIRE(placed.Position.y == Approx(-794.85f));
    REQUIRE(placed.Position.z == Approx(91.5f));
}

TEST_CASE("Body composition preserves receiver limb lengths and carries null-node descendants", "[body]")
{
    // External root + the actual 30-name hierarchy + a toe and a finger.
    constexpr std::array<int, 30> parents = {-1,0,1,2,3,4,2,6,7,1,9,10,11,12,11,14,15,16,17,15,19,14,11,22,23,24,25,23,27,22};
    std::array<Joint, 33> joints{};
    const Transform anchor{{1000.f, -500.f, 50.f}, glm::angleAxis(glm::radians(90.f), glm::vec3(0,0,1)), 2.f};
    joints[0].World = anchor;
    for (size_t i = 0; i < parents.size(); ++i)
    {
        joints[i+1].Parent = parents[i]+1;
        joints[i+1].Semantic = static_cast<int>(i);
    }
    // These lengths are from the inspected retail HKX, not the sender's stretched leg.
    joints[4].Local.Position = glm::vec3(-6.6151f, 0.f, 0.f); // thigh
    joints[5].Local.Position.z = 35.5953f; // calf
    joints[6].Local.Position.z = 27.9497f; // foot
    joints[31].Parent = 6;
    joints[31].Local.Position.z = 10.66f; // non-networked flattened toe
    joints[32].Parent = 26;
    joints[32].Local.Position.z = 4.f; // non-networked flattened finger
    auto pose = MakePose();
    for (auto& bone : pose.Bones) { bone.Position = {}; bone.Rotation = glm::quat(1,0,0,0); }
    pose.Bones[1].Position.z = 68.9113f;
    pose.Bones[5].Position.z = 400.f; // must not stretch the receiver's foot to the sender
    pose.Bones[5].Rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(1,0,0));
    REQUIRE(ComposeBody(joints, anchor, pose));
    REQUIRE_FALSE(joints[0].Owned);
    REQUIRE(joints[0].World.Position == anchor.Position);
    REQUIRE(joints[6].World.Position.z == Approx(50.f + 2.f*(68.9113f+35.5953f+27.9497f)));
    REQUIRE(joints[31].World.Position.x - joints[6].World.Position.x == Approx(21.32f).margin(0.0002));
    REQUIRE(joints[32].World.Position.z - joints[26].World.Position.z == Approx(8.f));
    for (const auto& joint : joints) REQUIRE(joint.Visit == 2);
    const auto toe = joints[31].World.Position;
    REQUIRE(ComposeBody(joints, anchor, pose));
    REQUIRE(glm::length(joints[31].World.Position-toe) < 0.0001f);
    joints[31].Parent = 31;
    REQUIRE_FALSE(ComposeBody(joints, anchor, pose));
    joints[31].Parent = 6;
    // An optional collapsed attachment does not stop the entire body. Its
    // descendants stay unwritten too, even if their own transforms are valid.
    joints[31].Local.Scale = 0.f;
    joints[32].Parent = 31;
    CompositionReport report{};
    REQUIRE(ComposeBody(joints, anchor, pose, &report));
    REQUIRE(report.PrunedNodes == 2);
    REQUIRE(joints[31].Pruned);
    REQUIRE(joints[32].Pruned);
    REQUIRE_FALSE(joints[31].Owned);
    REQUIRE_FALSE(joints[32].Owned);
    // Native skin coverage can make the same toe essential; don't warp skin by
    // treating every non-networked node as disposable.
    joints[32].RequiredForSkin = true;
    REQUIRE_FALSE(ComposeBody(joints, anchor, pose, &report));
    REQUIRE(report.Failure == CompositionFailure::RequiredTransform);
    joints[32].RequiredForSkin = false;
    joints[31].Local.Scale = 1.f;
    joints[5].Local.Scale = 0.f;
    REQUIRE_FALSE(ComposeBody(joints, anchor, pose, &report));
    REQUIRE(report.Failure == CompositionFailure::RequiredTransform);
}

TEST_CASE("Body origin follows network room drift independently of VRIK body compensation", "[body]")
{
    const glm::vec3 reference{1000.f, -400.f, 20.f};
    const auto yaw = glm::angleAxis(glm::radians(73.f), glm::vec3(0, 0, 1));
    // Exercise different body compensations rather than assuming a 0.6 factor.
    for (const float compensation : {-0.2f, 0.f, 0.6f, 1.f, 1.3f})
    for (const glm::vec3 drift : {glm::vec3(0.f), glm::vec3(17.25f, -23.5f, 0.f)})
    {
        const glm::vec3 hmd = reference + drift + glm::vec3(0.f, 0.f, 125.f);
        const auto frame = CaptureFrame(reference, hmd, yaw, 0.7f);
        REQUIRE(frame.Position == reference + drift);
        const Transform bodyRoot{reference + compensation * drift, yaw, 0.7f};
        const Transform hand{bodyRoot.Position + yaw * glm::vec3(21.f, 3.f, 70.f), yaw, 0.7f};
        const Transform weapon{hand.Position + yaw * glm::vec3(0.25f, 1.5f, 2.f), yaw, 0.7f};
        const auto relative = RelativeTo(frame, hand);
        // Receiver movement has already placed the actor at reference + drift.
        const Transform receiver{reference + drift, yaw, 0.7f};
        const auto replay = Compose(receiver, relative);
        REQUIRE(glm::length(replay.Position - hand.Position) < 0.0002f);
        const auto grip = RelativeTo(hand, weapon);
        REQUIRE(glm::length(Compose(replay, grip).Position - weapon.Position) < 0.0002f);
        // Subtracting the compensated visual root instead creates drift error.
        const auto wrong = Compose(receiver, RelativeTo(bodyRoot, hand));
        REQUIRE(glm::length((wrong.Position - hand.Position) - (1.f-compensation)*drift) < 0.0002f);

        // Exercise the actual body composer too: it uses Root/COM translation,
        // then receiver-local lengths, rather than each transmitted hand position.
        constexpr std::array<int, 30> parents = {-1,0,1,2,3,4,2,6,7,1,9,10,11,12,11,14,15,16,17,15,19,14,11,22,23,24,25,23,27,22};
        std::array<Joint, 31> joints{};
        joints[0].World = receiver;
        auto pose = MakePose();
        for (size_t i = 0; i < parents.size(); ++i)
        {
            joints[i+1].Parent = parents[i]+1;
            joints[i+1].Semantic = static_cast<int>(i);
            pose.Bones[i].Rotation = glm::quat(1,0,0,0);
        }
        pose.Bones[0].Position = RelativeTo(frame, bodyRoot).Position;
        const Transform com = Compose(bodyRoot, {{0,0,68}, glm::quat(1,0,0,0), 1});
        pose.Bones[1].Position = RelativeTo(frame, com).Position;
        joints[18].Local.Position = {21,3,32};
        const Transform measuredHand = Compose(com, joints[18].Local);
        REQUIRE(ComposeBody(joints, receiver, pose));
        REQUIRE(glm::length(joints[1].World.Position - bodyRoot.Position) < 0.0002f);
        REQUIRE(glm::length(joints[2].World.Position - com.Position) < 0.0002f);
        REQUIRE(glm::length(joints[18].World.Position - measuredHand.Position) < 0.0002f);
    }
}

TEST_CASE("Body ordering spans content generations and rejects delayed old content", "[body]")
{
    History history{};
    auto pose = MakePose(100, 1000);
    REQUIRE(history.Push(pose, 1000));
    pose.Generation = 2;
    pose.Sequence = 1;
    REQUIRE_FALSE(history.Push(pose, 1001)); // generation is not a session reset
    pose.Sequence = 101;
    REQUIRE(history.Push(pose, 1001));
    REQUIRE(history.Count == 1);
    auto delayedOld = MakePose(100, 1000);
    REQUIRE_FALSE(history.Push(delayedOld, 1002));
    REQUIRE(history.Generation == 2);
    // A destroyed actor/connection gets a fresh history, not a generation bypass.
    history = {};
    REQUIRE(history.Push(MakePose(1, 2000), 2000));
}

TEST_CASE("Equal body ticks cannot create a zero interpolation interval", "[body]")
{
    History history{};
    auto pose = MakePose(1, 1000);
    REQUIRE(history.Push(pose, 1000));
    pose.Sequence = 2;
    pose.Bones[1].Position.x = 20.f;
    REQUIRE(history.Push(pose, 1000));
    pose.Sequence = 3;
    pose.CaptureTick = 1100;
    pose.Bones[1].Position.x = 40.f;
    REQUIRE(history.Push(pose, 1100));
    Pose selected{};
    REQUIRE(history.Select(1300, selected) == Selection::Ready);
    REQUIRE(selected.Bones[1].Position.x == Approx(20.f));
    REQUIRE(history.Select(1350, selected) == Selection::Ready);
    REQUIRE(selected.Bones[1].Position.x == Approx(30.f));
}

TEST_CASE("Body relay stops immediately but bounds repeated losses and recovery", "[body]")
{
    RelayState state{};
    auto pose = MakePose(1, 1000);
    REQUIRE(state.Admit(pose, 1000) == Admission::Accepted);
    pose.Sequence = 2;
    pose.Mask = pose.GripMask = 0;
    REQUIRE(state.Admit(pose, 1000) == Admission::Accepted); // same-tick loss bypass
    pose.Sequence = 3;
    REQUIRE(state.Admit(pose, 1001) == Admission::Rate); // not unlimited stop spam
    pose.Sequence = 2;
    REQUIRE(state.Admit(pose, 1033) == Admission::Order);
    pose.Sequence = 4;
    REQUIRE(state.Admit(pose, 1033) == Admission::Accepted); // redundant loss
    pose = MakePose(5, 1034);
    REQUIRE(state.Admit(pose, 1034) == Admission::Rate);
    pose.Sequence = 6;
    REQUIRE(state.Admit(pose, 1066) == Admission::Accepted);
    pose.Generation = 2;
    pose.Sequence = 1;
    REQUIRE(state.Admit(pose, 1100) == Admission::Order);
    pose.Sequence = 7;
    REQUIRE(state.Admit(pose, 1065) == Admission::Clock);
}

TEST_CASE("Body producer dates actual captures and repeats loss without restamping stale data", "[body]")
{
    SourceStream stream{};
    SourceSample sample{MakePose(), 1, 1000, SourceState::Captured};
    Pose output{};
    REQUIRE(stream.Build(sample, 1020, 5020, output));
    REQUIRE(output.CaptureTick == 5000);
    REQUIRE(output.HasBody());
    const auto firstSequence = output.Sequence;
    REQUIRE_FALSE(stream.Build(sample, 1053, 5053, output)); // unchanged mailbox
    REQUIRE_FALSE(output.HasBody());
    // No new callback: a stale sample is silence, never a fresh pose or loss.
    REQUIRE_FALSE(stream.Build(sample, 1120, 5120, output));
    sample.Serial = 2;
    sample.State = SourceState::Lost;
    REQUIRE(stream.Build(sample, 1121, 5121, output));
    REQUIRE_FALSE(output.HasBody());
    REQUIRE(output.IsValid());
    REQUIRE(output.Sequence > firstSequence);
    REQUIRE_FALSE(stream.Build(sample, 1122, 5122, output));
    REQUIRE(stream.Build(sample, 1154, 5154, output));
    REQUIRE_FALSE(output.HasBody());
    REQUIRE(stream.Build(sample, 1187, 5187, output));
    const auto lastLossSequence = output.Sequence;
    REQUIRE_FALSE(stream.Build(sample, 1219, 5219, output)); // bounded to three
    // Explicit failed attempt also advances source order.
    sample.Serial = 3;
    sample.SteadyMs = 1220;
    sample.State = SourceState::TransientGap;
    REQUIRE_FALSE(stream.Build(sample, 1220, 5220, output));
    REQUIRE(stream.LastSerial == 3);
    sample.Serial = 4;
    sample.SteadyMs = 1253;
    sample.State = SourceState::Captured;
    sample.Body.Generation = 2;
    REQUIRE(stream.Build(sample, 1253, 5253, output));
    REQUIRE(output.HasBody());
    REQUIRE(output.Generation == 2);
    REQUIRE(output.Sequence > lastLossSequence);
    const auto beforeReconnect = output.Sequence;
    stream = {};
    REQUIRE(stream.Build(sample, 1253, 5253, output));
    REQUIRE(output.Sequence > beforeReconnect);
}

TEST_CASE("Body producer checks capture clock boundaries and loss retransmission reaches history", "[body]")
{
    SourceSample sample{MakePose(), 1, 1000, SourceState::Captured};
    SourceStream stream{};
    Pose output{};
    REQUIRE_FALSE(stream.Build(sample, 999, 5000, output)); // future steady sample
    REQUIRE_FALSE(stream.Build(sample, 1020, 20, output)); // zero/underflow capture tick
    REQUIRE_FALSE(stream.Build(sample, 1000, 0, output)); // no synchronized clock
    sample.Serial = 2; // a rejected capture is retired; recovery needs a new attempt
    REQUIRE(stream.Build(sample, 1000, 5000, output));
    RelayState relay{};
    History history{};
    REQUIRE(relay.Admit(output, 5000) == Admission::Accepted);
    REQUIRE(history.Push(output, 5000));
    sample.Serial = 3;
    sample.State = SourceState::Lost;
    REQUIRE(stream.Build(sample, 1001, 5001, output)); // immediate even within sender cadence; simulate lost packet
    REQUIRE(stream.Build(sample, 1066, 5066, output));
    REQUIRE(relay.Admit(output, 5066) == Admission::Accepted);
    REQUIRE(history.Push(output, 5066));
    REQUIRE(history.Select(5366, output) == Selection::SourceLost);
    // All three can still be lost on an unreliable lane; the stale timeout remains.
}

TEST_CASE("One failed capture or 400 ms stall does not clear a remote body", "[body]")
{
    SourceStream stream{};
    SourceSample sample{MakePose(), 1, 1000, SourceState::Captured};
    Pose output{};
    REQUIRE(stream.Build(sample, 1000, 5000, output));
    const auto firstSequence = output.Sequence;
    History history{};
    REQUIRE(history.Push(output, 5000));
    sample.Serial = 2;
    sample.State = SourceState::TransientGap;
    REQUIRE_FALSE(stream.Build(sample, 1033, 5033, output));
    REQUIRE(stream.HadBody);
    REQUIRE_FALSE(stream.Build(sample, 1400, 5400, output));
    REQUIRE(history.Select(5400, output) == Selection::Ready);
    REQUIRE(output.Sequence == firstSequence);
    sample.Serial = 3;
    sample.SteadyMs = 1400;
    sample.State = SourceState::Captured;
    REQUIRE(stream.Build(sample, 1400, 5400, output));
    REQUIRE(history.Push(output, 5400));
    REQUIRE(history.Count == 2);
    REQUIRE(history.Select(5400, output) == Selection::Ready);
    // A captured-but-late mailbox is also only silence.
    REQUIRE_FALSE(stream.Build(sample, 1800, 5800, output));
    REQUIRE(history.Select(5800, output) == Selection::Ready);
    REQUIRE_FALSE(stream.Build(sample, 2501, 6501, output));
    REQUIRE(history.Select(6501, output) == Selection::Stale); // safety bound remains
}

TEST_CASE("Body loss is a timed barrier and does not destroy the preceding segment", "[body]")
{
    History history{};
    auto before = MakePose(1, 1000);
    before.Bones[1].Position.x = 10;
    REQUIRE(history.Push(before, 1000));
    auto loss = MakePose(2, 1200);
    loss.Mask = loss.GripMask = 0;
    REQUIRE(history.Push(loss, 1200));
    auto after = MakePose(3, 1233);
    after.Bones[1].Position.x = 100;
    REQUIRE(history.Push(after, 1233));
    Pose output{};
    REQUIRE(history.Count == 3);
    REQUIRE(history.Select(1499, output) == Selection::Ready);
    REQUIRE(output.Bones[1].Position.x == Approx(10)); // do not interpolate toward loss/recovery
    REQUIRE(history.Select(1500, output) == Selection::SourceLost);
    REQUIRE_FALSE(output.HasBody());
    REQUIRE(history.Select(1532, output) == Selection::SourceLost);
    REQUIRE(history.Select(1533, output) == Selection::Ready);
    REQUIRE(output.Bones[1].Position.x == Approx(100));
    // Local OFF/disconnect/unsafe-actor removal still destroys this history
    // immediately; a delayed source event never overrides local release.
    history = {};
    REQUIRE(history.Select(1533, output) == Selection::Empty);
}

TEST_CASE("Actual retail reference bones pass and mod translation changes refuse", "[body]")
{
    for (const bool female : {false, true})
    {
        auto skeleton = RetailReferenceFixture(female);
        REQUIRE(ValidateReferenceSkeleton(skeleton) == ReferenceFailure::None);
        for (size_t i = 0; i < kBoneNames.size(); ++i)
        {
            const auto index = skeleton.SemanticIndices[i];
            REQUIRE(index >= 0);
            REQUIRE(skeleton.Bones[index].Name == kBoneNames[i]);
            auto changed = skeleton;
            changed.Bones[index].TranslationLocked = !changed.Bones[index].TranslationLocked;
            REQUIRE(ValidateReferenceSkeleton(changed) == ReferenceFailure::TranslationPolicy);
        }
        // Optional wire extras can be absent; neither skin coverage nor parent
        // compatibility is inferred from this test or the translation flags.
        auto optional = skeleton;
        const auto magic = optional.SemanticIndices[18];
        optional.Bones[magic].Name = "non-networked attachment";
        REQUIRE(ValidateReferenceSkeleton(optional) == ReferenceFailure::None);
        auto missing = skeleton;
        missing.Bones[missing.SemanticIndices[25]].Name = "unknown hand";
        REQUIRE(ValidateReferenceSkeleton(missing) == ReferenceFailure::MissingBody);
        auto duplicate = skeleton;
        duplicate.Bones.back().Name = duplicate.Bones.front().Name;
        REQUIRE(ValidateReferenceSkeleton(duplicate) == ReferenceFailure::Names);
        auto cycle = skeleton;
        cycle.Bones[3].Parent = 3;
        REQUIRE(ValidateReferenceSkeleton(cycle) == ReferenceFailure::Topology);
    }
}

TEST_CASE("Havok reference conversion preserves model units and refuses nonuniform scale", "[body]")
{
    const float position[4]{12.25f, -3.5f, 26.f, 0.f};
    const float rotation[4]{0.f, 0.f, 0.70710678f, 0.70710678f};
    float scale[4]{1.03f, 1.03f, 1.03f, 0.f};
    Transform output{};
    REQUIRE(ReferenceTransform(position, rotation, scale, output));
    REQUIRE(output.Position.x == Approx(12.25f));
    REQUIRE(output.Position.z == Approx(26.f));
    REQUIRE(output.Rotation.w == Approx(rotation[3]));
    REQUIRE(output.Rotation.z == Approx(rotation[2]));
    REQUIRE(output.Scale == Approx(1.03f));
    scale[1] = 1.2f;
    REQUIRE_FALSE(ReferenceTransform(position, rotation, scale, output));
    scale[1] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(ReferenceTransform(position, rotation, scale, output));
    scale[0] = scale[1] = scale[2] = 0.f;
    REQUIRE_FALSE(ReferenceTransform(position, rotation, scale, output));
}

TEST_CASE("Native world-axis frame retains turning and room-scale without applying actor angles twice", "[body]")
{
    const glm::vec3 reference{1100.f, -375.f, 80.f};
    const glm::vec3 hmd = reference + glm::vec3{19.25f, -31.5f, 123.f};
    for (const float angle : {-179.f, -91.f, 0.f, 73.f, 179.f})
    for (const float scale : {.7f, 1.f, 1.03f, 1.4f})
    {
        const auto measuredRotation = glm::angleAxis(glm::radians(angle), glm::vec3{0,0,1}) *
            glm::angleAxis(.42f, glm::vec3{1,0,0});
        // Actual post-VRIK world samples; neither actor pitch nor a synthesized
        // root matrix enters either native helper.
        const Transform root{reference + glm::vec3{13.25f, -20.f, .375f}, measuredRotation, scale};
        const auto com = Compose(root, {{0.f, 2.f, 69.f}, {1,0,0,0}, 1.f});
        const auto frame = SourceMovementFrame(reference, hmd, scale);
        const auto receiver = MovementFrame({hmd.x, hmd.y, reference.z}, scale);
        auto pose = MakePose();
        for (auto& bone : pose.Bones) { bone.Position = {}; bone.Rotation = measuredRotation; }
        const auto rootRelative = RelativeTo(frame, root);
        const auto comRelative = RelativeTo(frame, com);
        pose.Bones[0].Position = rootRelative.Position;
        pose.Bones[1].Position = comRelative.Position;
        constexpr std::array<int, 30> parents = {-1,0,1,2,3,4,2,6,7,1,9,10,11,12,11,14,15,16,17,15,19,14,11,22,23,24,25,23,27,22};
        std::array<Joint, 31> joints{};
        // Even a stale/differently pitched render root cannot turn the body a
        // second time: semantic rotations replace its composed directions.
        joints[0].World = {receiver.Position, glm::angleAxis(-1.1f, glm::vec3{0,1,0}), scale};
        for (size_t i = 0; i < parents.size(); ++i) { joints[i+1].Parent = parents[i]+1; joints[i+1].Semantic = static_cast<int>(i); }
        joints[18].Local.Position = {0.f, 0.f, 8.f};
        REQUIRE(ComposeBody(joints, receiver, pose));
        REQUIRE(glm::length(joints[1].World.Position - root.Position) < .0005f);
        REQUIRE(glm::length(joints[2].World.Position - com.Position) < .0005f);
        REQUIRE(std::abs(glm::dot(joints[18].World.Rotation, measuredRotation)) > .99999f);
        REQUIRE(glm::length(joints[18].World.Position - (com.Position + measuredRotation * glm::vec3{0,0,8.f*scale})) < .0005f);
    }
}

TEST_CASE("Native reference binding repairs poisoned lengths and rejects wrong parents or scale", "[body]")
{
    for (const bool female : {false, true})
    {
        auto reference = RetailReferenceFixture(female);
        REQUIRE(ValidateReferenceSkeleton(reference) == ReferenceFailure::None);
        std::vector<Joint> joints(reference.Bones.size() + 2);
        std::vector<std::string_view> names{"Scene Root"};
        for (size_t i = 0; i < reference.Bones.size(); ++i)
        {
            const auto& bone = reference.Bones[i];
            auto& joint = joints[i+1];
            names.push_back(bone.Name);
            joint.Parent = bone.Parent + 1;
            joint.Local = bone.Local;
            // Deliberately copy the historical error into the live input:
            // a warmed-up arm/leg measurement that was already stretched.
            if (bone.TranslationLocked) joint.Local.Position *= 1.25f;
            joint.Semantic = -1;
            for (size_t s = 0; s < kBoneNames.size(); ++s) if (bone.Name == kBoneNames[s]) joint.Semantic = static_cast<int>(s);
        }
        names.push_back("Tail Test Render-only Descendant");
        joints.back().Parent = reference.SemanticIndices[2] + 1;
        joints.back().Local.Position = {3.f, 4.f, 5.f};
        const auto before = joints;
        REQUIRE(BindReferenceLocals(joints, names, reference) == ReferenceFailure::None);
        for (size_t i = 0; i < reference.Bones.size(); ++i)
        {
            REQUIRE(joints[i+1].Local.Position == reference.Bones[i].Local.Position);
            REQUIRE(joints[i+1].Local.Rotation == before[i+1].Local.Rotation);
        }
        REQUIRE(joints.back().Local.Position == before.back().Local.Position);
        const auto hand = reference.SemanticIndices[17] + 1;
        auto broken = joints;
        broken[hand].Parent = 0;
        REQUIRE(BindReferenceLocals(broken, names, reference) == ReferenceFailure::Topology);
        broken = joints;
        broken[hand].Local.Scale *= 1.25f;
        REQUIRE(BindReferenceLocals(broken, names, reference) == ReferenceFailure::Transform);
    }
}

TEST_CASE("Body history never blends grips between different exact equipped items", "[body]")
{
    History history;
    auto sword = MakePose(100, 1000);
    auto bow = MakePose(101, 1033);
    bow.GripItems[0] = GameId{5, 123};
    REQUIRE(history.Push(sword, 1000));
    REQUIRE(history.Push(bow, 1033));
    Pose selected;
    REQUIRE(history.Select(1316, selected) == Selection::Ready);
    REQUIRE(selected.GripMask == 2);
    REQUIRE(selected.HasBody());
    REQUIRE(history.Select(1333, selected) == Selection::Ready);
    REQUIRE(selected.GripMask == 3);
    REQUIRE(selected.GripItems[0] == bow.GripItems[0]);
}

TEST_CASE("Native skin admission requires every retail finger twist and toe, not a nonempty partial cache", "[body]")
{
    for (const bool female : {false, true})
    {
        const auto reference = RetailReferenceFixture(female);
        REQUIRE(reference.Bones.size() == 99);
        std::vector<std::string_view> consumed;
        for (const auto& bone : reference.Bones) consumed.push_back(bone.Name);
        REQUIRE(CompleteSkinDetails(reference, consumed));
        size_t essential{};
        for (size_t i = 0; i < consumed.size(); ++i)
        {
            auto partial = consumed;
            partial.erase(partial.begin() + i);
            const bool detail = consumed[i].find("Finger") != std::string_view::npos ||
                consumed[i].find("ForearmTwist") != std::string_view::npos || consumed[i].find("Toe0") != std::string_view::npos;
            if (detail) { REQUIRE_FALSE(CompleteSkinDetails(reference, partial)); ++essential; }
        }
        REQUIRE(essential == 36);
        REQUIRE_FALSE(CompleteSkinDetails(reference, {}));
    }
}

TEST_CASE("Reference generation ignores effect churn while retaining body identity and scale changes", "[body]")
{
    constexpr std::array<int, 30> parents = {-1,0,1,2,3,4,2,6,7,1,9,10,11,12,11,14,15,16,17,15,19,14,11,22,23,24,25,23,27,22};
    std::vector<Joint> joints(32);
    std::vector<uint64_t> identities(32);
    for (size_t i = 0; i < identities.size(); ++i) identities[i] = 0x1000 + i * 0x100;
    for (size_t i = 0; i < parents.size(); ++i) { joints[i+1].Parent = parents[i]+1; joints[i+1].Semantic = static_cast<int>(i); }
    joints.back().Parent = 19; // effect below the optional right MagicNode
    const auto original = ReferenceContentIdentity(joints, identities, 1.03f);
    REQUIRE(original != 0);
    for (size_t frame = 0; frame < 120; ++frame)
    {
        identities.back() += 0x40;
        joints.back().Local.Scale = .1f + frame * .01f;
        joints.back().RequiredForSkin = frame % 2; // newly mapped effect consumer
        REQUIRE(ReferenceContentIdentity(joints, identities, 1.03f) == original);
    }
    // Inserting a cosmetic node changes traversal indices, not the identity of
    // any body parent. Generation is canonical semantic order, not walk order.
    joints.insert(joints.begin() + 1, Joint{});
    identities.insert(identities.begin() + 1, 0xfeed);
    for (size_t i = 2; i < joints.size(); ++i) if (joints[i].Parent >= 1) ++joints[i].Parent;
    REQUIRE(ReferenceContentIdentity(joints, identities, 1.03f) == original);
    const auto saved = identities[19]; // right hand (semantic 17) after insertion
    identities[19] ^= 0x400000;
    REQUIRE(ReferenceContentIdentity(joints, identities, 1.03f) != original);
    identities[19] = saved;
    joints[19].Local.Scale = 1.1f;
    REQUIRE(ReferenceContentIdentity(joints, identities, 1.03f) != original);
    joints[19].Local.Scale = 1.f;
    REQUIRE(ReferenceContentIdentity(joints, identities, 1.04f) != original);
    identities[0] ^= 0x100000; // external root replaced at the same actor ID
    REQUIRE(ReferenceContentIdentity(joints, identities, 1.03f) != original);
}

TEST_CASE("Body relay decision: the legacy flag refuses everything before any state exists, then admission resumes", "[body][relay]")
{
    RelayState state{};
    int created = 0;
    const auto get = [&]() -> RelayState& { ++created; return state; };
    auto pose = MakePose(1, 1000);

    // Server bUseLegacyHandPose on: nothing relayed, whatever the request, and no relay state made for it.
    REQUIRE(DecideRelay(true, true, pose, 1000, get) == RelayVerdict::Legacy);
    REQUIRE(DecideRelay(true, false, pose, 1000, get) == RelayVerdict::Legacy);
    REQUIRE(created == 0);

    // Off: the ownership, schema and clock checks come before the state too.
    REQUIRE(DecideRelay(false, false, pose, 1000, get) == RelayVerdict::NotOwner);
    auto broken = pose;
    broken.Mask |= 1u << 31;
    REQUIRE(DecideRelay(false, true, broken, 1000, get) == RelayVerdict::Malformed);
    REQUIRE(DecideRelay(false, true, pose, 1000 + kMaxAgeMs + 1, get) == RelayVerdict::Clock);
    auto future = pose;
    future.CaptureTick = 1000 + kMaxAgeMs + 1;
    REQUIRE(DecideRelay(false, true, future, 1000, get) == RelayVerdict::Clock);
    REQUIRE(created == 0);

    REQUIRE(DecideRelay(false, true, pose, 1000, get) == RelayVerdict::Relay);
    REQUIRE(created == 1);
    REQUIRE(DecideRelay(false, true, pose, 1033, get) == RelayVerdict::Order); // same sequence again

    // Toggled on at runtime (ToggleLegacyHandPose), then off again: refused while on, admitted after.
    pose.Sequence = 2;
    REQUIRE(DecideRelay(true, true, pose, 1033, get) == RelayVerdict::Legacy);
    REQUIRE(DecideRelay(false, true, pose, 1033, get) == RelayVerdict::Relay);
    pose.Sequence = 3;
    REQUIRE(DecideRelay(false, true, pose, 1034, get) == RelayVerdict::Rate);
}
