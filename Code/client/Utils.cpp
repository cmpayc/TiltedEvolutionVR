#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <PlayerCharacter.h>

namespace Utils
{

std::optional<uint32_t> GetServerId(entt::entity aEntity) noexcept
{
    const auto* pLocalComponent = World::Get().try_get<LocalComponent>(aEntity);
    const auto* pRemoteComponent = World::Get().try_get<RemoteComponent>(aEntity);
    const auto* pObjectComponent = World::Get().try_get<ObjectComponent>(aEntity);

    uint32_t serverId = -1;
    if (pLocalComponent)
        serverId = pLocalComponent->Id;
    else if (pRemoteComponent)
        serverId = pRemoteComponent->Id;
    else if (pObjectComponent)
        serverId = pObjectComponent->Id;
    else
    {
        const auto* pFormIdComponent = World::Get().try_get<FormIdComponent>(aEntity);
        spdlog::debug("{}: This entity has neither a local or remote component: {:X}, form id: {:X}", __FUNCTION__, to_integral(aEntity), pFormIdComponent ? pFormIdComponent->Id : 0);
        return std::nullopt;
    }

    return {serverId};
}

std::optional<entt::entity> FindEntityByServerId(const uint32_t aServerId) noexcept
{
    const auto localView = World::Get().view<LocalComponent>();
    const auto localIt = std::find_if(
        localView.begin(), localView.end(),
        [localView, aServerId](const entt::entity aEntity) { return localView.get<LocalComponent>(aEntity).Id == aServerId; });
    if (localIt != localView.end())
        return *localIt;

    const auto remoteView = World::Get().view<RemoteComponent>();
    const auto remoteIt = std::find_if(
        remoteView.begin(), remoteView.end(),
        [remoteView, aServerId](const entt::entity aEntity) { return remoteView.get<RemoteComponent>(aEntity).Id == aServerId; });
    if (remoteIt != remoteView.end())
        return *remoteIt;

    const auto objectView = World::Get().view<ObjectComponent>();
    const auto objectIt = std::find_if(
        objectView.begin(), objectView.end(),
        [objectView, aServerId](const entt::entity aEntity) { return objectView.get<ObjectComponent>(aEntity).Id == aServerId; });
    if (objectIt != objectView.end())
        return *objectIt;

    return std::nullopt;
}

std::optional<ActorOwnershipToken> GetLocalOwnershipToken(const uint32_t aFormId) noexcept
{
    auto view = World::Get().view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view, aFormId](const entt::entity aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });
    if (it == view.end())
        return std::nullopt;

    const auto& localComponent = view.get<LocalComponent>(*it);
    if (localComponent.OwnershipEpoch == 0)
        return std::nullopt;

    return ActorOwnershipToken{localComponent.Id, localComponent.OwnershipEpoch};
}

std::optional<ActorOwnershipToken> GetRemoteOwnershipToken(const uint32_t aFormId) noexcept
{
    auto view = World::Get().view<FormIdComponent, RemoteComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view, aFormId](const entt::entity aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });
    if (it == view.end())
        return std::nullopt;

    const auto& remoteComponent = view.get<RemoteComponent>(*it);
    if (remoteComponent.OwnershipEpoch == 0)
        return std::nullopt;

    return ActorOwnershipToken{remoteComponent.Id, remoteComponent.OwnershipEpoch};
}

void ShowHudMessage(const TiltedPhoques::String& acMessage)
{
    using TShowHudMessage = void(const char*, const char*, bool);

    POINTER_SKYRIMSE(TShowHudMessage, s_showHudMessage, 52933);

    s_showHudMessage(acMessage.c_str(), nullptr, false);
}

#if TP_SKYRIMVR
glm::vec2 GetRoomscaleOffset() noexcept
{
    // HmdNode's slot on PlayerCharacter and NiAVObject's world transform translate. Both measured, see
    // PROGRESS.md session 9. HandPoseService checks the first against the node's name once per session and says
    // so loudly if it has moved, so it is not re-checked here.
    constexpr size_t cHmdNodeOffset = 0x570;
    constexpr size_t cWorldTranslate = 0xA0;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return glm::vec2(0.f);

    auto* pHmd = *reinterpret_cast<NiAVObject* const*>(reinterpret_cast<const uint8_t*>(pPlayer) + cHmdNodeOffset);
    if (!pHmd)
        return glm::vec2(0.f);

    // Guarded rather than trusted, because the offset above was measured rather than documented.
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pHmd, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return glm::vec2(0.f);

    const auto& cHmd = *reinterpret_cast<const glm::vec3*>(reinterpret_cast<const uint8_t*>(pHmd) + cWorldTranslate);

    return glm::vec2(cHmd.x - pPlayer->position.x, cHmd.y - pPlayer->position.y);
}
#endif

} // namespace Utils
