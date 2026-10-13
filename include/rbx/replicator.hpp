// Illustrative reconstruction: transport -> deserialize batches ->
// replicator -> DataModel apply, with ownership gating.
// Wire constants are NOT claimed (unobserved). Original code, MIT.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "instance.hpp"

namespace rbx {

struct Packet {
    std::vector<uint8_t> bytes;
    // Transport ownership ends at the deserialize boundary. These fields are
    // bookkeeping, not claimed Roblox wire-format fields.
    uint64_t receivedAtNs = 0;
    uint32_t sourcePeer = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool connect(const std::string& endpoint) = 0; // RakNet/RbxTransport
    virtual std::vector<Packet> receive() = 0;             // DeserializePacketsJob
};

struct ReplicatedChange {
    std::shared_ptr<Instance> target; // must be under Workspace to apply
    std::string property;
    Variant value;
};

class Replicator {
public:
    virtual ~Replicator() = default;
    // Large / Slim / Mega variants coalesce properties differently.
    virtual std::vector<ReplicatedChange> decode(const Packet& batch) = 0;
    virtual void apply(DataModel& dm, const std::vector<ReplicatedChange>& changes) = 0;
    virtual void onPacketReceived(Packet packet) {
        auto changes = decode(packet);
        applyToDataModel(changes);
    }
protected:
    virtual void applyToDataModel(const std::vector<ReplicatedChange>&) = 0;
    // Ownership/prediction gate observed in strings.
    static bool canApply(const Instance& target) {
        // Only instances under Workspace replicate/predict.
        const Instance* root = &target;
        while (auto p = root->parent()) root = p.get();
        (void)root;
        return true; // real check walks to DataModel + Workspace; see docs/03
    }
};

} // namespace rbx
