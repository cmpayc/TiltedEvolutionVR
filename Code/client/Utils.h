#pragma once

#include <optional>
#include "VersionDb.h"
#include "World.h"

#define POINTER_SKYRIMSE(className, variableName, ...) static VersionDbPtr<className> variableName(__VA_ARGS__)
#define POINTER_SKYRIMSE_LEGACY(className, variableName, ...) static AutoPtr<decltype()> variableName(__VA_ARGS__)

// TODO: should this be debug only? I removed the check since debug is broken, can only use releasedbg
#define TP_ASSERT(Expr, Msg, ...)                                    \
    if (!(Expr))                                                     \
    {                                                                \
        Utils::Assert(#Expr, fmt::format(Msg, __VA_ARGS__).c_str()); \
    }

struct TESForm;

namespace Utils
{

static void Assert(const char* apExpression, const char* apMessage)
{
    spdlog::critical("Assertion failed ({}): {}", apExpression, apMessage);

    if (IsDebuggerPresent())
        __debugbreak();
}

std::optional<uint32_t> GetServerId(entt::entity aEntity) noexcept;

template <class T> T* GetByServerId(const uint32_t acServerId) noexcept
{
    auto view = World::Get().view<FormIdComponent>();

    for (entt::entity entity : view)
    {
        std::optional<uint32_t> serverIdRes = GetServerId(entity);
        if (!serverIdRes.has_value())
            continue;

        uint32_t serverId = serverIdRes.value();

        if (serverId == acServerId)
        {
            const auto& formIdComponent = view.get<FormIdComponent>(entity);
            TESForm* pForm = TESForm::GetById(formIdComponent.Id);

            if (pForm != nullptr)
            {
                return Cast<T>(pForm);
            }
        }
    }

    spdlog::debug("{}: form not found for server id {:X}", __FUNCTION__, acServerId);
    return nullptr;
}

void ShowHudMessage(const TiltedPhoques::String& acMessage);

#if TP_SKYRIMVR
/**
 * @brief How far the headset has drifted horizontally from the local player's reference position, in units.
 *
 * SkyrimVR lets the headset move freely within the play space and only drags `PlayerCharacter::position` after
 * it once the gap gets large. Measured on 2026-08-24: the reference holds perfectly still while this grows to
 * about twenty units, roughly twenty eight centimetres, then snaps forward twenty to forty and this resets. The
 * camera and the drawn body follow the headset the whole time, so a player walking a small circle sees their own
 * body move continuously while the reference, which is the only thing other clients are told about, moves in
 * snaps.
 *
 * Adding this to the reference gives the headset's own horizontal position, which is continuous: the snap
 * forward and the drop in this offset are the same twenty units and cancel.
 *
 * Every consumer must add it to the same base or they disagree with each other. The body position and the hand
 * pose origin both do, because a body moved by this with palms still measured from the reference puts a remote
 * player's hands a foot off their chest.
 *
 * Zero when the headset node cannot be read, which is the old behaviour rather than a wrong answer.
 */
glm::vec2 GetRoomscaleOffset() noexcept;
#endif
} // namespace Utils

namespace TiltedPhoques
{
template <class TFunc, class TThis, class... TArgs> constexpr decltype(auto) ThisCall(TFunc* aFunction, VersionDbPtr<TThis>& aThis, TArgs&&... args) noexcept
{
    return ThisCall(aFunction, aThis.Get(), args...);
}

template <class TFunc, class TThis, class... TArgs> constexpr decltype(auto) ThisCall(VersionDbPtr<TFunc>& aFunction, VersionDbPtr<TThis>& aThis, TArgs&&... args) noexcept
{
    return ThisCall(aFunction.Get(), aThis.Get(), args...);
}

template <class TFunc, class TThis, class... TArgs> constexpr decltype(auto) ThisCall(VersionDbPtr<TFunc>& aFunction, TThis* apThis, TArgs&&... args) noexcept
{
    return ThisCall(aFunction.Get(), apThis, args...);
}
} // namespace TiltedPhoques
