// DataModel session root + service classes.
// Shapes follow the documented game API surface (class names, members,
// events); lifecycle mirrors observed behavior (per-session root,
// BringUp/Close, job-queued startup). Original code, MIT.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "instance.hpp"

namespace rbx {

// ---- value types seen across descriptors ----
struct Vector3 { float x = 0, y = 0, z = 0; };
struct Color3 { float r = 0, g = 0, b = 0; };
struct BrickColor { uint32_t palette = 0; };

// ---- forward services ----
class Workspace;
class Players;
class Lighting;
class ReplicatedStorage;
class ReplicatedFirst;
class ServerStorage;
class ServerScriptService;
class StarterPlayer;
class StarterGui;
class StarterPack;
class Teams;
class SoundService;
class Chat;
class TextChatService;
class HttpService;
class MarketplaceService;
class DataStoreService;
class CollectionService;
class TweenService;
class RunService;
class DebrisService;
class PathfindingService;
class UserInputService;
class ScriptContext;
class TestService;

class DataModel : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }

    template <typename T> std::shared_ptr<T> service();
    bool shuttingDown() const { return shuttingDown_; }
    void setShuttingDown() { shuttingDown_ = true; }
private:
    bool shuttingDown_ = false;
};

// Process-wide factory/manager behind call-once.
class DataModelFactory {
public:
    static DataModelFactory& instance() {
        static DataModelFactory f;
        return f;
    }
    std::shared_ptr<DataModel> bringUp(const std::string& clientId);
    void close(std::shared_ptr<DataModel> dm);
    std::shared_ptr<DataModel> current() const { return current_.lock(); }
private:
    std::weak_ptr<DataModel> current_;
};

// ---- World ----
class BasePart : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    Vector3 position, size = {4, 1, 2};
    bool anchored = false, canCollide = true;
    // Only parts under Workspace simulate or replicate.
    EventDescriptor touched{"Touched"};
};

class Model : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};

class Workspace : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    float gravity = 196.2f;
    bool streamingEnabled = false;
    size_t streamingTargetRadius = 1024;
    // struct Gravity etc. omitted for brevity — see reflection registry demo
};

class Terrain : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    Vector3 regionSize; // voxel Grid_NEW regions replicate in chunks
};

// ---- Players ----
class Player : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    uint64_t userId = 0;
    std::string displayName;
    EventDescriptor characterAdded{"CharacterAdded"};
};

class Players : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    int maxPlayers = 50;
    bool characterAutoLoads = true;
    std::shared_ptr<Player> localPlayer; // client only
    EventDescriptor playerAdded{"PlayerAdded"};
    EventDescriptor playerRemoving{"PlayerRemoving"};
};

class Teams : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};

// ---- Rendering/atmosphere ----
class Lighting : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    float brightness = 2.0f, clockTime = 14.0f, geographicLatitude = 0.0f;
    bool globalShadows = true;
    Color3 ambient{0.5f, 0.5f, 0.5f};
};

// ---- Replication containers ----
class ReplicatedStorage : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};
class ReplicatedFirst : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool removeDefaultLoadingScreen = true;
};
class ServerStorage : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};
class ServerScriptService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};

// ---- Starter packs ----
class StarterPlayer : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    float characterWalkSpeed = 16.0f, characterJumpPower = 50.0f;
    bool characterUseJumpPower = true;
};
class StarterGui : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool screenOrientationFollows = true;
};
class StarterPack : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};

// ---- Audio ----
class Sound : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    std::string soundId;
    float volume = 0.5f;
    bool playing = false, looped = false;
    EventDescriptor ended{"Ended"};
};
class SoundService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    float ambientReverb = 0.0f;
    bool respectFilteringEnabled = true;
};

// ---- Chat ----
class TextChannel : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    EventDescriptor messageReceived{"MessageReceived"};
};
class TextChatService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    int maxTextLength = 200;
};
class Chat : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool loadStringEnabled = false; // server filtering path
};

// ---- Web / commerce / persistence ----
class HttpService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool httpEnabled = true;
};
class MarketplaceService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};
class DataStore : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    std::string name, scope = "global";
};
class DataStoreService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    std::shared_ptr<DataStore> getDataStore(const std::string& name,
                                            const std::string& scope = "global");
};

// ---- Gameplay helpers ----
class CollectionService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    void addTag(std::shared_ptr<Instance>, const std::string&);
    bool hasTag(std::shared_ptr<Instance>, const std::string&);
};
class Tween : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    float playbackState = 0; // Playing/Paused/Completed enum in full impl
    EventDescriptor completed{"Completed"};
};
class TweenService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    std::shared_ptr<Tween> create(std::shared_ptr<Instance>, float duration);
};
class RunService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool isClient = true, isServer = false, isStudio = false;
    EventDescriptor heartbeat{"Heartbeat"};
    EventDescriptor stepped{"Stepped"};
    EventDescriptor renderStepped{"RenderStepped"};
};
class DebrisService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    void addItem(std::shared_ptr<Instance>, float lifetime);
};
class PathfindingService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
};
class UserInputService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool touchEnabled = false, gamepadConnected = false;
    EventDescriptor inputBegan{"InputBegan"};
};
class TestService : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    bool autoRuns = true;
    std::string error;
};

} // namespace rbx
