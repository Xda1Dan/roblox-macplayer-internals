// Demo: bring up a DataModel, wire services, simulate + replicate a touch.
// Build: make datamodel_demo
#include <cstdio>
#include "rbx/datamodel.hpp"
#include "rbx/scheduler.hpp"

namespace rbx {
// Factory lifecycle, mirroring BringUp/CloseDataModel.
std::shared_ptr<DataModel> DataModelFactory::bringUp(const std::string& clientId) {
    auto dm = std::make_shared<DataModel>();
    dm->setName("Game-" + clientId);
    dm->addService(std::make_shared<Workspace>());
    dm->addService(std::make_shared<Players>());
    dm->addService(std::make_shared<Lighting>());
    dm->addService(std::make_shared<ReplicatedStorage>());
    dm->addService(std::make_shared<RunService>());
    current_ = dm;
    return dm;
}
void DataModelFactory::close(std::shared_ptr<DataModel> dm) {
    dm->setShuttingDown();
    if (current_.lock() == dm) current_.reset();
}
} // namespace rbx

int main() {
    auto dm = rbx::DataModelFactory::instance().bringUp("MacDesktopClient");
    auto ws = dm->getService<rbx::Workspace>();
    auto players = dm->getService<rbx::Players>();
    auto run = dm->getService<rbx::RunService>();
    std::printf("services: ws=%d players=%d run=%d\n",
                !!ws, !!players, !!run);

    // World content under Workspace (only this replicates/simulates).
    auto plate = std::make_shared<rbx::BasePart>();
    plate->setName("Baseplate");
    plate->anchored = true;
    ws->addChild(plate);
    auto player = std::make_shared<rbx::Player>();
    player->setName("Builderman");
    players->addChild(player);

    // A script-style connection: Touched -> run on the scheduler.
    auto sched = rbx::TaskScheduler::create();
    plate->touched.connections.push_back(
        [&](const rbx::Tuple&) {
            std::printf("%s touched by %s\n",
                        plate->name().c_str(), player->name().c_str());
        });
    sched->submit([&] { plate->touched.connections[0]({}); });
    sched->submit([&] { sched->shutdown(); });
    while (sched->runOne()) {
    }

    rbx::DataModelFactory::instance().close(dm);
    std::printf("shutdown=%d\n", (int)dm->shuttingDown());
    return 0;
}
