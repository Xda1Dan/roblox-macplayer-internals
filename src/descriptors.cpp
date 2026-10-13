// ClassDescriptor definitions for every documented class.
// Names match the binary's service/class roster. Build: part of all demos.
#include "rbx/datamodel.hpp"
#include "rbx/script_context.hpp"

#define DESC(cls) \
    const rbx::ClassDescriptor rbx::cls::kDescriptor{#cls, nullptr, {}, {}, {}}

namespace rbx {
const ClassDescriptor DataModel::kDescriptor{"DataModel", nullptr, {}, {}, {}};
} // namespace rbx

DESC(BasePart);
DESC(Model);
DESC(Workspace);
DESC(Terrain);
DESC(Player);
DESC(Players);
DESC(Teams);
DESC(Lighting);
DESC(ReplicatedStorage);
DESC(ReplicatedFirst);
DESC(ServerStorage);
DESC(ServerScriptService);
DESC(StarterPlayer);
DESC(StarterGui);
DESC(StarterPack);
DESC(Sound);
DESC(SoundService);
DESC(TextChannel);
DESC(TextChatService);
DESC(Chat);
DESC(HttpService);
DESC(MarketplaceService);
DESC(DataStore);
DESC(DataStoreService);
DESC(CollectionService);
DESC(Tween);
DESC(TweenService);
DESC(RunService);
DESC(DebrisService);
DESC(PathfindingService);
DESC(UserInputService);
DESC(TestService);
DESC(ScriptContext);
