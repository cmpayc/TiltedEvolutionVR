#include <TiltedOnlinePCH.h>
#include <Services/BodyPoseService.h>
#include <Services/TransportService.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Messages/RequestBodyPose.h>
#include <Messages/NotifyBodyPose.h>
#include <Structs/BodyPoseHistory.h>
#include <Structs/ServerSettings.h>
#include <NetImmerse/BodyNative.h>
#include <Services/BodyPostPass.h>
#include <Services/PoseSyncMode.h>
#include <Havok/BodyReferenceSkeleton.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <World.h>
#include <Components.h>
#include <Utils.h>
#include <mutex>
#include <unordered_map>

using namespace BodyTracking;
namespace
{
// How often the BODY status lines are written, and only for a remote or a source that did something.
constexpr uint64_t kStatusIntervalMs = 10000;

void LogNativeDetails(const char* scope, uint32_t actor, const NativeDiagnostic& detail)
{
    spdlog::info("BODY native_detail scope={} actor={:X} reason={} geometry={} slot={} bone={} bone_ptr={:X} world_ptr={:X} bind_reason={} bind_bone={} live_scale={} reference_scale={} live_parent={} reference_parent={}",
        scope, actor, detail.Reason, detail.Geometry, detail.Slot, detail.Bone, detail.BonePointer, detail.WorldPointer,
        static_cast<int>(detail.Binding.Failure), detail.Binding.Bone, detail.Binding.LiveScale, detail.Binding.ReferenceScale, detail.Binding.LiveParent, detail.Binding.ReferenceParent);
    if (detail.Skin.Operand)
        spdlog::info("BODY skin_operand scope={} actor={:X} operand={} unreadable={} geometry={} skin_ptr={:X} data_ptr={:X} count={} bones_ptr={:X} worlds_ptr={:X} partition_ptr={:X} slot={} bone_ptr={:X} world_ptr={:X}",
            scope, actor, detail.Skin.Operand, detail.Skin.Unreadable, detail.Geometry, detail.Skin.Skin, detail.Skin.Data, detail.Skin.Count, detail.Skin.Bones, detail.Skin.Worlds, detail.Skin.Partition, detail.Skin.Slot, detail.Skin.Bone, detail.Skin.World);
}
bool InSpace(Actor& actor)
{
    auto* player = PlayerCharacter::Get();
    if (!player || !actor.GetParentCellEx() || !player->GetParentCellEx()) return false;
    if (actor.GetParentCellEx() == player->GetParentCellEx()) return true;
    const auto* world = actor.GetWorldSpace();
    return world && world == player->GetWorldSpace();
}
bool SafeActor(Actor* actor)
{
    return actor && !actor->IsDead() && !actor->actorState.IsBleedingOut() && InSpace(*actor);
}
}
struct BodyPoseService::Impl
{
    struct Remote
    {
        uint32_t FormId{};
        uintptr_t Root{};
        uint64_t Content{1};
        History Buffer;
        std::unordered_map<uint64_t, uint32_t> GripIds;
        ReferenceSkeleton Reference;
        uint64_t RetryAt{};
        uint8_t Retries{};
        uint64_t NativeRetryAt{};
        uint8_t NativeRetries{};
        uint64_t ReportedGeneration{};
        bool RefusalLogged{}; // a refusal streak logs its native detail once; a successful write ends the streak
        ReferenceFailure ReferenceError{ReferenceFailure::Manager};
        NativeFailure LastNative{};
        // BODY status window, reset each time the status line is written.
        uint64_t Frames{}, Writes{}, Refusals{}, Rebuilds{}, PublishFailures{}, GripApplied{};
        // Post-pass counters taken from the registry just before a release would reset them, so a window that
        // ended in a release still reports what happened in it.
        PostPass::WindowCounters Released{};
    };
    World& Game;
    TransportService& Transport;
    std::mutex Mutex;
    std::unordered_map<uint32_t, Remote> Remotes;
    SourceStream Source;
    uint32_t LocalId{};
    uint64_t Tick{}, UpdatedAt{}, LastStatus{}, LastSerial{}, SlotCheckAt{};
    // Source status window.
    uint64_t Sent{}, Captured{}, Gaps{}, Lost{}, CaptureMicrosMax{};
    bool Connected{}, Receive{true};
    // The server's bUseLegacyHandPose: no capture, send, receive or write while it is on.
    bool Legacy{};
    Impl(World& game, TransportService& transport) : Game(game), Transport(transport) {}

    bool Active() const noexcept { return PoseSyncMode::BodyLane(PoseSyncMode::Decide(Connected, Legacy)); }

    // Folds this remote's post-pass window into its status window (the registry resets a slot's window on release).
    static void TakePostPass(uint32_t id, Remote& remote) noexcept
    {
#if TP_SKYRIMVR
        PostPass::WindowCounters window{};
        if (!PostPass::Global().TakeCounters(id, window)) return;
        auto& total = remote.Released;
        total.Calls += window.Calls;
        total.Writes += window.Writes;
        total.Reapplies += window.Reapplies;
        total.Skips += window.Skips;
        total.Stale += window.Stale;
        total.Moved += window.Moved;
        total.Busy += window.Busy;
        total.Overlap += window.Overlap;
        total.PublishBusy += window.PublishBusy;
        total.Publishes += window.Publishes;
        total.Unmapped += window.Unmapped;
        total.MaxMicros = std::max(total.MaxMicros, window.MaxMicros);
        total.Threads = std::max(total.Threads, window.Threads);
#else
        (void)id;
        (void)remote;
#endif
    }
    // Every post-pass release goes through here, under Mutex, so the status window keeps what the slot counted.
    void ReleasePostPass(uint32_t id, Remote& remote) noexcept
    {
        TakePostPass(id, remote);
#if TP_SKYRIMVR
        PostPass::Global().Release(id);
#endif
    }
    /**
     * The BODY status line for one remote, then a fresh window. Written every kStatusIntervalMs for a remote that
     * wrote or faulted, and once more, with the reason, just before a remote is dropped (disconnect, receiver or
     * mode change, update stall, removal), so nothing counted in a window disappears unreported.
     */
    static void Report(uint32_t id, Remote& remote, const char* reason) noexcept
    {
        TakePostPass(id, remote);
        const auto& window = remote.Released;
#if TP_SKYRIMVR
        const bool active = PostPass::Active();
#else
        const bool active = false;
#endif
        const bool faults = remote.Refusals || window.Skips || window.Stale || window.Moved || window.Busy || window.Overlap || remote.PublishFailures || window.PublishBusy;
        if (remote.Writes || faults)
            spdlog::info("BODY status remote={} actor={:X} reason={} frames={} writes={} refusals={} rebuilds={} reference={} native={} grips={} pp_calls={} pp_reapplies={} pp_writes={} pp_skips={} pp_stale={} pp_moved={} pp_busy={} pp_overlap={} pp_threads={} pp_max_us={} publishes={} publish_fail={} unmapped={} active={}",
                id, remote.FormId, reason, remote.Frames, remote.Writes, remote.Refusals, remote.Rebuilds, static_cast<int>(remote.ReferenceError), static_cast<int>(remote.LastNative),
                remote.GripApplied, window.Calls, window.Reapplies, window.Writes, window.Skips, window.Stale, window.Moved, window.Busy, window.Overlap, window.Threads, window.MaxMicros,
                window.Publishes, remote.PublishFailures + window.PublishBusy, window.Unmapped, active ? 1 : 0);
        remote.Frames = remote.Writes = remote.Refusals = remote.Rebuilds = remote.PublishFailures = remote.GripApplied = 0;
        remote.Released = {};
    }
    // Drops every remote and every post-pass slot, reporting each remote's pending window first.
    void ClearRemotes(const char* reason) noexcept
    {
        for (auto& [id, remote] : Remotes) Report(id, remote, reason);
        Remotes.clear();
#if TP_SKYRIMVR
        PostPass::Global().ReleaseAll();
#endif
    }
    void LogMode() const noexcept
    {
        spdlog::info("Pose sync mode: {} (server bUseLegacyHandPose={})", Legacy ? "legacy" : "body", Legacy ? 1 : 0);
    }
};
BodyPoseService::BodyPoseService(entt::dispatcher& dispatcher, World& game, TransportService& transport)
    : m_impl(std::make_unique<Impl>(game, transport))
{
    m_update = dispatcher.sink<UpdateEvent>().connect<&BodyPoseService::OnUpdate>(this);
    m_connected = dispatcher.sink<ConnectedEvent>().connect<&BodyPoseService::OnConnected>(this);
    m_disconnected = dispatcher.sink<DisconnectedEvent>().connect<&BodyPoseService::OnDisconnected>(this);
    m_removed = dispatcher.sink<ActorRemovedEvent>().connect<&BodyPoseService::OnRemoved>(this);
    m_body = dispatcher.sink<NotifyBodyPose>().connect<&BodyPoseService::OnBody>(this);
    m_settings = dispatcher.sink<ServerSettings>().connect<&BodyPoseService::OnSettings>(this);
}
BodyPoseService::~BodyPoseService() { EnableBodyCapture(false); }

void BodyPoseService::OnConnected(const ConnectedEvent&)
{
    std::lock_guard lock(m_impl->Mutex);
    auto& impl = *m_impl;
    impl.Connected = true;
    // The authentication response installs the settings before ConnectedEvent is raised.
    impl.Legacy = impl.Game.GetServerSettings().UseLegacyHandPose;
    impl.ClearRemotes("connect");
    impl.Source = {};
    impl.LocalId = 0;
    impl.Tick = 0;
    EnableBodyCapture(impl.Active());
    impl.LogMode();
}
void BodyPoseService::OnDisconnected(const DisconnectedEvent&)
{
    std::lock_guard lock(m_impl->Mutex);
    auto& impl = *m_impl;
    impl.Connected = false;
    impl.ClearRemotes("disconnect");
    impl.Source = {};
    impl.LocalId = 0;
    impl.Tick = 0;
    EnableBodyCapture(false);
}
void BodyPoseService::OnSettings(const ServerSettings& settings)
{
    std::lock_guard lock(m_impl->Mutex);
    auto& impl = *m_impl;
    if (!impl.Connected || impl.Legacy == settings.UseLegacyHandPose) return;
    impl.Legacy = settings.UseLegacyHandPose;
    impl.ClearRemotes("mode_change");
    impl.Source = {};
    EnableBodyCapture(impl.Active());
    impl.LogMode();
}
void BodyPoseService::SetReceiveEnabled(bool enabled)
{
    std::lock_guard lock(m_impl->Mutex);
    if (m_impl->Receive == enabled) return;
    m_impl->Receive = enabled;
    m_impl->ClearRemotes(enabled ? "receiver_on" : "receiver_off"); // no pre-OFF body can resume on ON
    spdlog::info("BODY receiver {}: histories/reference generations cleared", enabled ? "ON" : "OFF");
}
void BodyPoseService::OnRemoved(const ActorRemovedEvent& event)
{
    std::lock_guard lock(m_impl->Mutex);
    for (auto& [id, remote] : m_impl->Remotes)
        if (remote.FormId == event.FormId)
        {
            Impl::Report(id, remote, "removed");
#if TP_SKYRIMVR
            PostPass::Global().Release(id);
#endif
        }
    std::erase_if(m_impl->Remotes, [&](const auto& item) { return item.second.FormId == event.FormId; });
}
void BodyPoseService::OnBody(const NotifyBodyPose& message)
{
    auto& impl = *m_impl;
    if (!message.Body.IsValid()) return;
    // PlayerComponent is populated by Tilted's IsPlayer spawn message. Never
    // infer player-ness from a reused actor pointer, race or generated form ID.
    auto view = impl.Game.view<RemoteComponent, FormIdComponent, PlayerComponent>();
    const auto found = std::find_if(view.begin(), view.end(), [&](auto entity) { return view.get<RemoteComponent>(entity).Id == message.Id; });
    if (found == view.end()) return;
    const auto form = view.get<FormIdComponent>(*found).Id;
    std::lock_guard lock(impl.Mutex);
    if (!impl.Active() || !impl.Receive) return;
    auto& remote = impl.Remotes[message.Id];
    if (remote.FormId != form)
    {
        if (remote.FormId) Impl::Report(message.Id, remote, "actor_changed");
        impl.ReleasePostPass(message.Id, remote);
        remote = {};
        remote.FormId = form;
    }
    for (size_t h = 0; h < message.Body.Grips.size(); ++h)
        if (message.Body.GripMask & (1u << h))
        {
            if (remote.GripIds.size() >= History::kCapacity * 2) remote.GripIds.clear();
            remote.GripIds[message.Body.GripItems[h].LogFormat()] = impl.Game.GetModSystem().GetGameId(message.Body.GripItems[h]);
        }
    remote.Buffer.Push(message.Body, message.ServerTick);
}
void BodyPoseService::OnUpdate(const UpdateEvent&)
{
    auto& impl = *m_impl;
    std::lock_guard lock(impl.Mutex);
    if (!impl.Active()) return;
    const auto now = BodySteadyMs();
    impl.UpdatedAt = now;
    impl.Tick = impl.Game.GetTick();
    if (!impl.LocalId)
    {
        auto* player = PlayerCharacter::Get();
        auto view = impl.Game.view<FormIdComponent>();
        for (auto entity : view)
            if (player && view.get<FormIdComponent>(entity).Id == player->formID)
            {
                if (const auto id = Utils::GetServerId(entity)) impl.LocalId = *id;
                break;
            }
    }
    // A missing 3D may stop HIGGS invoking us altogether. This is a positive
    // native loss observation on update, unlike a callback merely arriving late.
    auto* sourcePlayer = PlayerCharacter::Get();
    if (!sourcePlayer || !sourcePlayer->GetNiNode()) ReportBodyCaptureLoss();
    auto source = ReadBodyCapture();
    for (size_t h = 0; h < source.LocalGripItems.size(); ++h)
        if (source.Body.GripMask & (1u << h))
            if (!impl.Game.GetModSystem().GetServerModId(source.LocalGripItems[h], source.Body.GripItems[h])) source.Body.GripMask &= ~(1u << h);
    if (source.Serial != impl.LastSerial)
    {
        impl.LastSerial = source.Serial;
        if (source.State == SourceState::Captured)
        {
            ++impl.Captured;
            impl.CaptureMicrosMax = std::max<uint64_t>(impl.CaptureMicrosMax, source.CaptureMicros);
        }
        else if (source.State == SourceState::Lost) ++impl.Lost;
        else ++impl.Gaps;
    }
    RequestBodyPose request;
    if (impl.LocalId && impl.Source.Build(source, now, impl.Tick, request.Body))
    {
        request.Id = impl.LocalId;
        impl.Transport.Send(request);
        ++impl.Sent;
    }
    // Reconcile against the live ECS only on update. The render phase receives
    // numeric form IDs and copied skeleton data, never an ECS iterator.
    auto view = impl.Game.view<RemoteComponent, FormIdComponent, PlayerComponent>();
    for (auto it = impl.Remotes.begin(); it != impl.Remotes.end();)
    {
        auto& remote = it->second;
        const auto found = std::find_if(view.begin(), view.end(), [&](auto entity)
        {
            return view.get<RemoteComponent>(entity).Id == it->first && view.get<FormIdComponent>(entity).Id == remote.FormId;
        });
        if (found == view.end())
        {
            Impl::Report(it->first, remote, "gone");
#if TP_SKYRIMVR
            PostPass::Global().Release(it->first);
#endif
            it = impl.Remotes.erase(it);
            continue;
        }
        auto* actor = Cast<Actor>(TESForm::GetById(remote.FormId));
        auto* root = SafeActor(actor) ? actor->GetNiNode() : nullptr;
        const auto address = reinterpret_cast<uintptr_t>(root);
        if (remote.Root != address)
        {
            impl.ReleasePostPass(it->first, remote); // the old tree must stop matching before the new root is adopted
            remote.Buffer = {};
            remote.Reference = {};
            remote.Content = 1;
            remote.Retries = 0;
            remote.RetryAt = 0;
            remote.NativeRetryAt = 0;
            remote.NativeRetries = 0;
            ++remote.Rebuilds;
        }
        remote.Root = address;
        if (root && now >= remote.RetryAt)
        {
            ReferenceDiagnostic details;
            const auto oldGraph = remote.Reference.GraphIdentity;
            const auto oldSkeleton = remote.Reference.SkeletonIdentity;
            remote.ReferenceError = ReadBodyReferenceSkeleton(*actor, root, remote.Content, remote.Reference, &details, true);
            if (remote.ReferenceError == ReferenceFailure::None)
            {
                if (oldGraph && (oldGraph != remote.Reference.GraphIdentity || oldSkeleton != remote.Reference.SkeletonIdentity)) remote.Buffer = {};
                remote.Retries = 0;
                remote.RetryAt = 0; // cheap identity probe; no repeat array/name snapshot
            }
            else
            {
                static constexpr uint64_t backoff[]{250, 1000, 4000, 5000};
                remote.RetryAt = now + backoff[std::min<size_t>(remote.Retries, 3)];
                if (remote.Retries < 3) ++remote.Retries;
                if (remote.ReportedGeneration != remote.Content)
                {
                    remote.ReportedGeneration = remote.Content;
                    spdlog::warn("BODY reference refused actor={:X} generation={} reason={} storedHolder={:X} actorBase={:X} interface={:X} storedRoot={:X} expectedRoot={:X}",
                        remote.FormId, remote.Content, static_cast<int>(remote.ReferenceError), details.StoredHolder, details.ActorBase,
                        details.HolderInterface, details.StoredRoot, details.ExpectedRoot);
                }
            }
        }
        ++it;
    }
#if TP_SKYRIMVR
    // Functional, not a report: a vtable slot another module replaced after startup turns re-apply off on all three
    // thunks (the plain writer then runs alone). Checked once a second whatever is being written or logged.
    if (now - impl.SlotCheckAt >= 1000)
    {
        impl.SlotCheckAt = now;
        if (PostPass::Installed()) PostPass::SlotsIntact();
    }
#endif
    if (now - impl.LastStatus < kStatusIntervalMs) return;
    impl.LastStatus = now;
    if (impl.Sent || impl.Captured || impl.Gaps || impl.Lost)
        spdlog::info("BODY source sent={} captured={} gaps={} lost={} capture_us_max={} state={} reason={} native={}",
            impl.Sent, impl.Captured, impl.Gaps, impl.Lost, impl.CaptureMicrosMax, static_cast<int>(source.State), static_cast<int>(source.Failure), source.NativeDetail);
    impl.Sent = impl.Captured = impl.Gaps = impl.Lost = impl.CaptureMicrosMax = 0;
    for (auto& [id, remote] : impl.Remotes) Impl::Report(id, remote, "interval");
}
std::unordered_set<uint32_t> BodyPoseService::Render(const std::function<bool(const glm::vec3&, float)>& visible, std::unordered_set<uint32_t>* apReady)
{
    std::unordered_set<uint32_t> owned;
    auto& impl = *m_impl;
    std::lock_guard lock(impl.Mutex);
    if (!impl.Active() || !impl.Receive || !impl.Tick) return owned;
    const auto now = BodySteadyMs();
    // Main update can stall; advance its copied clock locally, never restamp a
    // captured body. If update disappears altogether, immediately release.
    if (now < impl.UpdatedAt || now - impl.UpdatedAt > kMaxAgeMs)
    {
        impl.ClearRemotes("update_stall");
        return owned;
    }
    const auto tick = impl.Tick + now - impl.UpdatedAt;
    for (auto& [id, remote] : impl.Remotes)
    {
        const auto render = [&]
        {
            Pose pose;
            if (remote.Buffer.Select(tick, pose) != Selection::Ready)
            {
                impl.ReleasePostPass(id, remote); // no live source: the hook must not keep restoring an old pose
                return;
            }
            // Ready is reported separately: the legacy path holds off for one bounded window from the first Ready
            // frame (first acquire), and only a real write below refreshes ownership.
            if (apReady) apReady->insert(id);
            if (now < remote.NativeRetryAt) return;
            NativeBody body;
            const auto refused = [&]
            {
                ++remote.Refusals;
                impl.ReleasePostPass(id, remote);
                if (!remote.RefusalLogged)
                {
                    remote.RefusalLogged = true;
                    LogNativeDetails("remote", remote.FormId, body.Diagnostic());
                }
                if (remote.LastNative == NativeFailure::Changed || remote.LastNative == NativeFailure::Unreadable)
                {
                    remote.NativeRetryAt = now + kSendIntervalMs;
                    return; // a torn invocation cannot buy a five-second blackout
                }
                static constexpr uint64_t backoff[]{250, 1000, 4000, 5000};
                remote.NativeRetryAt = now + backoff[std::min<size_t>(remote.NativeRetries, 3)];
                if (remote.NativeRetries < 3) ++remote.NativeRetries;
            };
            auto* actor = Cast<Actor>(TESForm::GetById(remote.FormId));
            auto* root = SafeActor(actor) ? actor->GetNiNode() : nullptr;
            if (!root || reinterpret_cast<uintptr_t>(root) != remote.Root)
            {
                remote.Buffer = {};
                remote.Reference = {};
                impl.ReleasePostPass(id, remote);
                return;
            }
            const glm::vec3 position{actor->position.x, actor->position.y, actor->position.z};
            // Same proven cone/sphere guard used by the legacy writer.
            if (!visible(position + glm::vec3{0.f, 0.f, 90.f}, 100.f)) return;
            remote.LastNative = body.Read(root, true);
            if (remote.LastNative != NativeFailure::None) { refused(); return; }
            const auto content = body.Content();
            if (remote.Content != content)
            {
                remote.Content = content;
                remote.Reference = {};
                remote.Retries = 0;
                remote.RetryAt = 0;
                ++remote.Rebuilds;
                impl.ReleasePostPass(id, remote);
                return; // snapshot original lengths in update before any write
            }
            if (remote.Reference.Bones.empty() || remote.Reference.Generation != remote.Content) return;
            remote.LastNative = body.Bind(remote.Reference);
            if (remote.LastNative != NativeFailure::None) { refused(); return; }
            std::array<uint32_t, 2> items{};
            // These numeric IDs were mapped on receipt. No World/ECS/mod map
            // is consulted by this native render loop.
            for (size_t h = 0; h < items.size(); ++h)
                if (pose.GripMask & (1u << h))
                    if (auto found = remote.GripIds.find(pose.GripItems[h].LogFormat()); found != remote.GripIds.end()) items[h] = found->second;
            float error{};
            GripReport grips;
            const auto anchor = MovementFrame(position, body.RootWorld().Scale);
            PostPass::Snapshot snapshot;
            remote.LastNative = body.Apply(pose, anchor, items, error, &grips, &snapshot);
            remote.GripApplied += std::popcount(grips.Applied);
            if (remote.LastNative == NativeFailure::None)
            {
                owned.insert(id);
                remote.NativeRetries = 0;
                remote.NativeRetryAt = 0;
                remote.RefusalLogged = false;
                ++remote.Writes;
#if TP_SKYRIMVR
                const auto published = PostPass::Global().Publish(id, snapshot);
                if (published == PostPass::PublishResult::NoSlot || published == PostPass::PublishResult::TooLarge) ++remote.PublishFailures;
#endif
            }
            else refused();
        };
        render();
        ++remote.Frames;
    }
    return owned;
}
