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
