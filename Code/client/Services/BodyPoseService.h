#pragma once

#include <memory>
#include <functional>
#include <unordered_set>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct ActorRemovedEvent;
struct NotifyBodyPose;
struct ServerSettings;

// The only body transport owner. Native capture owns no pointer to this service.
// Render is invoked from HandPoseService's existing late writer, before arms.
struct BodyPoseService
{
    BodyPoseService(entt::dispatcher& aDispatcher, World& aWorld, TransportService& aTransport);
    ~BodyPoseService();
    TP_NOCOPYMOVE(BodyPoseService);
    void SetReceiveEnabled(bool aEnabled);
    // Returns the remotes the body wrote this frame; apReady (optional) receives the remotes whose history selected a pose.
    std::unordered_set<uint32_t> Render(const std::function<bool(const glm::vec3&, float)>& aVisible, std::unordered_set<uint32_t>* apReady = nullptr);
private:
    void OnUpdate(const UpdateEvent&);
    void OnConnected(const ConnectedEvent&);
    void OnDisconnected(const DisconnectedEvent&);
    void OnRemoved(const ActorRemovedEvent&);
    void OnBody(const NotifyBodyPose&);
    // The server's bUseLegacyHandPose changing while connected: the body lane stops or starts at once.
    void OnSettings(const ServerSettings&);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    entt::scoped_connection m_update, m_connected, m_disconnected, m_removed, m_body, m_settings;
};
