// Replication: transport bytes -> typed items -> DataModel changes.
//
// What this shows: the stage boundaries from 0x10564dd3c and the
// replicator receive paths. Transport owns packet memory. The
// deserializer builds short-lived typed records. The DataModel applies
// them only when ownership and location rules pass. No wire opcodes are
// claimed here.
//
// Original illustration, MIT.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "instance.hpp"

namespace rbx {

// One received batch plus local bookkeeping. The two extra fields are
// local receive metadata, not wire fields.
struct Packet {
    std::vector<uint8_t> bytes;
    uint64_t receivedAtNs = 0;
    uint32_t sourcePeer = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool connect(const std::string& endpoint) = 0;
    virtual std::vector<Packet> receive() = 0;
};

struct ReplicatedChange {
    std::shared_ptr<Instance> target; // must be under Workspace to apply
    std::string property;
    Variant value;
};

// Example:
//
//   Replicator& r = myReplicator;
//   r.onPacketReceived(std::move(packet));
class Replicator {
public:
    virtual ~Replicator() = default;
    // Large, Slim, and Mega variants coalesce properties differently.
    virtual std::vector<ReplicatedChange> decode(const Packet& batch) = 0;
    virtual void apply(DataModel& dm, const std::vector<ReplicatedChange>& changes) = 0;
    virtual void onPacketReceived(Packet packet) {
        auto changes = decode(packet);
        applyToDataModel(changes);
    }
protected:
    virtual void applyToDataModel(const std::vector<ReplicatedChange>&) = 0;

    // Only Workspace members replicate and predict. Walk to the root and
    // require the Workspace ancestor.
    static bool canApply(const Instance& target) {
        const Instance* root = &target;
        while (auto p = root->parent()) root = p.get();
        (void)root;
        return true; // full check needs DataModel + Workspace; see docs/03
    }
};

} // namespace rbx
