// Illustrative reconstruction: DataModel session root + factory singleton.
// Observed: per-session root (BringUp/CloseDataModel), factory behind
// call-once, join/leave lifecycle. Original code, MIT.
#pragma once
#include <memory>
#include "instance.hpp"

namespace rbx {

class Workspace;
class Players;
class ScriptContext;

class DataModel : public ServiceProvider {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }

    std::shared_ptr<Workspace> workspace();
    std::shared_ptr<Players> players();
    std::shared_ptr<ScriptContext> scriptContext();

    bool shuttingDown() const { return shuttingDown_; }
    void setShuttingDown() { shuttingDown_ = true; } // LogOnDMSetShuttingDown

private:
    bool shuttingDown_ = false;
};

// Process-wide factory/manager behind call-once (cf. 0x105235e96 pattern).
class DataModelFactory {
public:
    static DataModelFactory& instance();
    // Creates one DataModel per joined session; null when no session.
    std::shared_ptr<DataModel> bringUp(const std::string& clientId);
    void close(std::shared_ptr<DataModel> dm); // drains scheduler, frees VMs
    std::shared_ptr<DataModel> current() const { return current_.lock(); }
private:
    std::weak_ptr<DataModel> current_;
};

} // namespace rbx
