#include <TiltedOnlinePCH.h>

#include <Services/ActorValueService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Games/References.h>
#include <Components.h>

#include <Events/UpdateEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HealthChangeEvent.h>

#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/RequestDeathStateChange.h>

#include <misc/ActorValueOwner.h>

#include <Forms/TESNPC.h>

namespace
{
/**
 * @brief Whether the game would refuse to let this actor die of its wounds.
 *
 * Both flags, because they are set in different places and either one is enough. The base form carries what the
 * mod author marked, which is where a vanilla essential NPC gets it, and the actor carries the runtime flag that
 * Actor::SetEssentialEx writes when this client makes a body essential itself.
 */
bool IsEssentialActor(Actor& aActor) noexcept
{
    if (aActor.IsEssential())
        return true;

    /**
     * Up the template chain, not just the base form the reference points at.
     *
     * A levelled NPC, which is most of the ones worth fighting, carries almost nothing of its own. Its traits
     * come from the template it was made from, and the essential flag is one of them, so reading only the leaf
     * reports a guard or a bandit chief as ordinary and kills it.
     */
    TESNPC* pBase = Cast<TESNPC>(aActor.baseForm);

    for (uint32_t depth = 0; pBase && depth < 8; ++depth)
    {
        if (pBase->actorData.IsEssential())
            return true;

        pBase = pBase->GetTemplateBase();
    }

    return false;
}
} // namespace

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_world.on_construct<LocalComponent>().connect<&ActorValueService::OnLocalComponentAdded>(this);
    m_dispatcher.sink<DisconnectedEvent>().connect<&ActorValueService::OnDisconnected>(this);
    m_dispatcher.sink<ActorRemovedEvent>().connect<&ActorValueService::OnActorRemoved>(this);
    m_dispatcher.sink<UpdateEvent>().connect<&ActorValueService::OnUpdate>(this);
    m_dispatcher.sink<NotifyActorValueChanges>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_dispatcher.sink<NotifyActorMaxValueChanges>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_dispatcher.sink<HealthChangeEvent>().connect<&ActorValueService::OnHealthChange>(this);
    m_dispatcher.sink<NotifyHealthChangeBroadcast>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_dispatcher.sink<NotifyDeathStateChange>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::CreateActorValuesComponent(const entt::entity aEntity, Actor* apActor) noexcept
{
    auto& actorValuesComponent = m_world.emplace_or_replace<ActorValuesComponent>(aEntity);

    for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
    {
        float value = apActor->GetActorValue(i);
        actorValuesComponent.CurrentActorValues.ActorValuesList.insert({i, value});
        float maxValue = apActor->GetActorPermanentValue(i);
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList.insert({i, maxValue});
    }
}

void ActorValueService::OnLocalComponentAdded(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    const auto& formIdComponent = aRegistry.get<FormIdComponent>(aEntity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (pActor != NULL)
    {
        auto& localComponent = aRegistry.get<LocalComponent>(aEntity);
        localComponent.IsDead = pActor->IsDead();
        localComponent.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        CreateActorValuesComponent(aEntity, pActor);
    }
}

void ActorValueService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    // TODO: this crashes sometimes, no clue why
    m_world.clear<ActorValuesComponent>();

    // Nobody's word to keep any more, and a form id from the last session names nothing in the next one.
    m_remoteBleedingOut.clear();
}

void ActorValueService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();
    const uint32_t formId = acEvent.FormId;

    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [view, formId](auto entity)
        {
            const auto& formIdComponent = view.get<FormIdComponent>(entity);

            return formIdComponent.Id == formId;
        });

    if (it != std::end(view))
        m_world.remove<ActorValuesComponent>(*it);

    // Dropped with the actor, so the map does not grow for a session and does not carry an opinion about a form
    // id the game may hand to something else later.
    m_remoteBleedingOut.erase(formId);
}

void ActorValueService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunSmallHealthUpdates();
    RunDeathStateUpdates();
    RunRemoteDownStateUpdates();
    RunActorValuesUpdates();
}

void ActorValueService::RunRemoteDownStateUpdates() noexcept
{
    if (m_remoteBleedingOut.empty())
        return;

    static std::chrono::steady_clock::time_point lastRunTimePoint;
    constexpr auto cDelayBetweenRuns = 250ms;

    const auto cNow = std::chrono::steady_clock::now();
    if (cNow - lastRunTimePoint < cDelayBetweenRuns)
        return;

    lastRunTimePoint = cNow;

    // Re-looked up rather than bound, because iterating this map hands out const values. Same shape as
    // ApplyCachedWeaponDraws, which works around it the same way.
    for (auto& [cFormId, _] : m_remoteBleedingOut)
    {
        RemoteDownState& state = m_remoteBleedingOut[cFormId];

        Actor* pActor = Cast<Actor>(TESForm::GetById(cFormId));

        if (!pActor || pActor->IsDead())
            continue;

        // Latched while it lasts, because it does not last long enough to be read when it is needed. Our copy
        // clears this flag as soon as its health returns, which happens before the owner has decided to get up.
        if (pActor->actorState.IsBleedingOut())
        {
            state.WeSawItDown = true;
            continue;
        }

        // Nothing to undo, or the owner still has it down and our copy should stay where it is.
        if (!state.WeSawItDown || state.OwnerHasItDown)
            continue;

        /**
         * The body went down here and its owner has it back on its feet, so ours has to be put back on its own.
         *
         * The state agrees by this point and only the pose is wrong: the flag cleared quietly when health
         * returned, without anything playing a get-up, so the body is left lying where the knockdown put it and
         * nothing in the game is going to lift it. A remote actor does not run the combat state that would.
         *
         * Respawn is Resurrect followed by the reference reset, which is what the death path has always used to
         * put an actor back on its feet.
         */
        state.WeSawItDown = false;

        spdlog::info("Actor {:X} went down here and its owner has it up again, standing it up", cFormId);

        pActor->Respawn();
    }
}

void ActorValueService::BroadcastActorValues() noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent, ActorValuesComponent>();

    for (auto entity : view)
    {
        auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);

        if (!pActor)
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);

        RequestActorValueChanges requestValueChanges;
        requestValueChanges.Id = localComponent.Id;
        requestValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;
        RequestActorMaxValueChanges requestMaxValueChanges;
        requestMaxValueChanges.Id = localComponent.Id;
        requestMaxValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;

        bool isPlayer = pActor->GetExtension() && pActor->GetExtension()->IsPlayer();

        for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
        {
            if (isPlayer && i == ActorValueInfo::kDragonSouls)
                continue;
            
            float newValue = pActor->GetActorValue(i);
            float oldValue = actorValuesComponent.CurrentActorValues.ActorValuesList[i];
            if (newValue != oldValue)
            {
                requestValueChanges.Values.insert({i, newValue});
                actorValuesComponent.CurrentActorValues.ActorValuesList[i] = newValue;
            }

            float newMaxValue = pActor->GetActorPermanentValue(i);
            float oldMaxValue = actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i];
            if (newMaxValue != oldMaxValue)
            {
                requestMaxValueChanges.Values.insert({i, newMaxValue});
                actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i] = newMaxValue;
            }
        }

        if (requestValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestValueChanges);
        }

        if (requestMaxValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestMaxValueChanges);
        }
    }
}

void ActorValueService::OnHealthChange(const HealthChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto hitteeIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.HitteeId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (hitteeIt == std::end(view))
    {
        spdlog::warn("Health change event form id component not found, form id: {:X}", acEvent.HitteeId);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*hitteeIt);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    if (acEvent.DeltaHealth > -1.0f && acEvent.DeltaHealth < 1.0f)
    {
        if (m_smallHealthChanges.find(serverId) == m_smallHealthChanges.end())
            m_smallHealthChanges[serverId] = acEvent.DeltaHealth;
        else
            m_smallHealthChanges[serverId] += acEvent.DeltaHealth;
        return;
    }

    RequestHealthChangeBroadcast requestHealthChange;
    requestHealthChange.Id = serverId;
    requestHealthChange.DeltaHealth = acEvent.DeltaHealth;

    m_transport.Send(requestHealthChange);

    spdlog::debug("Sent out delta health through collection: {:X}:{:f}", serverId, acEvent.DeltaHealth);
}

void ActorValueService::RunSmallHealthUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    if (!m_smallHealthChanges.empty())
    {
        for (auto& value : m_smallHealthChanges)
        {
            RequestHealthChangeBroadcast requestHealthChange;
            requestHealthChange.Id = value.first;
            requestHealthChange.DeltaHealth = value.second;

            m_transport.Send(requestHealthChange);

            spdlog::debug("Sent out delta health through timer, {:X}:{:f}", value.first, value.second);
        }

        m_smallHealthChanges.clear();
    }
}

void ActorValueService::RunDeathStateUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto localView = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : localView)
    {
        const auto& formIdComponent = localView.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        auto& localComponent = localView.get<LocalComponent>(entity);

        const bool cIsDead = pActor->IsDead();

        // Reported alongside death, and it is the half that actually changes for an essential actor. See
        // RequestDeathStateChange::IsBleedingOut.
        const bool cIsBleedingOut = pActor->actorState.IsBleedingOut();

        if (cIsDead != localComponent.IsDead || cIsBleedingOut != localComponent.IsBleedingOut)
        {
            const bool cIsDeadChanged = cIsDead != localComponent.IsDead;

            localComponent.IsDead = cIsDead;
            localComponent.IsBleedingOut = cIsBleedingOut;

            RequestDeathStateChange requestChange;
            requestChange.Id = localComponent.Id;
            requestChange.OwnershipEpoch = localComponent.OwnershipEpoch;
            requestChange.IsDead = cIsDead;
            requestChange.IsBleedingOut = cIsBleedingOut;

            m_transport.Send(requestChange);

            // The owner is the only client that knows what happened to an actor, and until now it said so to
            // nobody but the wire. These are rare and everything else about a body hangs off them, so both
            // ends of the message get a line.
            spdlog::info("Local actor {:X} is now {}{}, telling the server (server id {:X})", pActor->formID, cIsDead ? "dead" : "alive", cIsBleedingOut ? " and down" : cIsDeadChanged ? "" : " and back on its feet", localComponent.Id);
        }
    }
}

void ActorValueService::RunActorValuesUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    BroadcastActorValues();
}

void ActorValueService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) const noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.Id);
        return;
    }

    const float newHealth = pActor->GetActorValue(ActorValueInfo::kHealth) + acMessage.DeltaHealth;
    pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, newHealth);

    const float health = pActor->GetActorValue(ActorValueInfo::kHealth);
    if (!pActor->IsDead() && health <= 0.f)
    {
        ActorExtension* pExtension = pActor->GetExtension();

        /**
         * Running out of health is not the same thing as dying, and this used to treat them as one.
         *
         * An essential actor does not die when its health reaches zero. The game puts it into bleedout and it
         * stands back up a while later, which is what the client that felled it sees. Killing it here does not
         * reproduce that, it overrides it: Actor::Kill is KillImpl with force, so it goes straight through the
         * protection the game applies, and the body then stays dead on this client for good.
         *
         * Nothing ever undoes it either, which is the half that makes this permanent rather than merely early.
         * A death is only relayed when the owner's own IsDead changes, and on the owner's machine nothing died:
         * Papyrus IsDead is false throughout a bleedout. So no death state change is sent, and none is sent when
         * it gets up. Reported on 2026-08-25 as an NPC that stood up for the player who felled it and stayed
         * dead for everyone else.
         *
         * The owner is the authority on whether an actor died, and it already reports it: RunDeathStateUpdates
         * sends the transition and OnDeathStateChange applies it in both directions. So leaving an essential
         * actor alone here costs nothing. If it really does die, the message says so and it dies then.
         */
        const bool cEssential = IsEssentialActor(*pActor);

        if (!pExtension->IsPlayer() && !cEssential)
            pActor->Kill();
    }

    // TODO(cosideci): find fix for player health sync so this can be used again
    /*
    if (pActor->GetExtension()->IsRemotePlayer())
        World::Get().GetOverlayService().SetPlayerHealthPercentage(pActor->formID);
    */
}

void ActorValueService::OnActorValueChanges(const NotifyActorValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [&acMessage, view](entt::entity entity)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        return remote.Id == acMessage.Id && acMessage.OwnershipEpoch != 0 && remote.OwnershipEpoch == acMessage.OwnershipEpoch;
    });

    if (itor == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        // Syncing dragon souls triggers "Dragon soul collected" event
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        if (key == ActorValueInfo::kHealth)
        {
            /**
             * Health from the owner, for everything except a player.
             *
             * This used to be dropped for every actor, and dropping it is why a downed NPC never got back up.
             * The only other thing that moves a remote actor's health is NotifyHealthChangeBroadcast, which
             * carries the deltas from hits, so health on this client could go down and had no way back. An
             * essential NPC that bleeds out and recovers on its owner's machine therefore stayed at zero here
             * for good, which reads as dead and was reported as exactly that on 2026-08-25: the owner saw it
             * stand up, everybody else saw a body.
             *
             * The owner has been sending the real value once a second the whole time. BroadcastActorValues
             * diffs every actor value and health is in there; only this end threw it away.
             *
             * Players are still skipped. Their health sync has a known fault of its own, recorded in the TODO
             * in OnHealthChangeBroadcast, and this is not the change that fixes it.
             */
            if (pActor->GetExtension() && pActor->GetExtension()->IsPlayer())
                continue;

            /**
             * Health is applied and nothing is concluded from it.
             *
             * Standing an actor up from here was tried on 2026-08-25 and is wrong twice over. It fires on
             * whatever health happens to arrive, so it lifted a body while its owner's copy was still down at
             * 0.8 health, and it left the body in a pose nobody asked for. Whether an actor is down is not a
             * fact about its health, it is a fact about its owner, and the owner now says so directly. See
             * OnDeathStateChange.
             */
        }

        spdlog::debug("Actor value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        if (key == ActorValueInfo::kStamina || key == ActorValueInfo::kMagicka || key == ActorValueInfo::kHealth)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, key, value);
            continue;
        }
        pActor->SetActorValue(key, value);
    }
}

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    const auto it = std::find_if(std::begin(view), std::end(view), [&acMessage, view](entt::entity entity)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        return remote.Id == acMessage.Id && acMessage.OwnershipEpoch != 0 && remote.OwnershipEpoch == acMessage.OwnershipEpoch;
    });

    if (it == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
}

void ActorValueService::OnDeathStateChange(const NotifyDeathStateChange& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    const auto it = std::find_if(std::begin(view), std::end(view), [&acMessage, view](entt::entity entity)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        return remote.Id == acMessage.Id && acMessage.OwnershipEpoch != 0 && remote.OwnershipEpoch == acMessage.OwnershipEpoch;
    });

    if (it == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    ActorExtension* pExtension = pActor->GetExtension();
    // Players should never be killed
    if (pExtension->IsPlayer())
        return;

    if (pActor->IsDead() != acMessage.IsDead)
    {
        // The other end of the line in RunDeathStateUpdates. Between the two, a body that is dead on one screen
        // and alive on another can be traced to whichever client stopped agreeing, which was guesswork before.
        spdlog::info("Owner says actor {:X} is {}, applying it here (server id {:X})", pActor->formID, acMessage.IsDead ? "dead" : "alive again", acMessage.Id);

        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

        return;
    }

    /**
     * The owner's word on whether the body is down, kept rather than acted on once.
     *
     * This is the case IsDead could never carry. An essential actor knocked down and recovered is alive at both
     * ends throughout, so the branch above never fires, while this client puts its own copy on the floor off
     * the health it is sent and cannot get it up again: finishing a bleedout is a decision the local game takes
     * from combat state that a remote actor never runs.
     *
     * Acting on the message as it lands does not work either, and that is measured rather than assumed. The
     * owner's NPC went down at 18:03:40 and stood up at 18:03:46, while this client's copy did not reach zero
     * health until 18:03:51, so the get-up arrived while our body was still standing, matched nothing, and was
     * gone by the time it mattered. RunRemoteDownStateUpdates enforces what was said last, for as long as it
     * stands, which covers a copy that goes down five seconds late as easily as one that is already down.
     */
    m_remoteBleedingOut[pActor->formID].OwnerHasItDown = acMessage.IsBleedingOut;
}
