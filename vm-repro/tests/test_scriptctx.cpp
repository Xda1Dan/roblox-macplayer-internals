// test_scriptctx.cpp — verification of the ScriptContext-layer reproduction.
//
// Semantics under test come from the decompiled binary functions cited in
// ../research/vm-research/scriptcontext.md. The VM boundary is simulated; the point
// is the decision logic, ordering, and exact fatal messages.
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "rbxvm/scriptctx.hpp"

using namespace rbxvm;

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL: %s:%d: ", __FILE__, __LINE__);\
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                              \
        }                                                    \
    } while (0)

#define CHECK_THROWS_MSG(expr, needle, tag)                                    \
    do {                                                                       \
        bool caught = false;                                                   \
        try { expr; }                                                          \
        catch (const std::runtime_error& e) {                                  \
            caught = true;                                                     \
            CHECK(std::string(e.what()).find(needle) != std::string::npos,     \
                  "%s: wrong message: %s", tag, e.what());                     \
        }                                                                      \
        CHECK(caught, "%s: expected a fatal error", tag);                      \
    } while (0)

namespace {

// A scriptable VM boundary: queued return statuses + a clock we control.
class FakeVm : public sctx::VmBoundary {
public:
    std::vector<int> resume_results{0};
    size_t resume_calls = 0, resume_error_calls = 0;
    bool checkstack_ok = true, push_ok = true;
    std::vector<std::string> pushed_errors;
    double clock = 100.0;
    double clock_step = 0.0; // optional advance per now() call

    int resume(int) override {
        ++resume_calls;
        return resume_results.at(std::min(resume_calls, resume_results.size()) - 1);
    }
    int resumeError() override { ++resume_error_calls; return resume_results.at(0); }
    bool checkStack(int) override { return checkstack_ok; }
    bool pushError(const char* msg, size_t len) override {
        if (!push_ok) return false;
        pushed_errors.emplace_back(msg, len);
        return true;
    }
    double now() override {
        double t = clock;
        clock += clock_step;
        return t;
    }
};

} // namespace

static void testFreshResume() {
    sctx::ScriptContextState ctx;
    FakeVm vm;
    vm.clock_step = 0.5; // time passes between the guard's clock reads
    vm.resume_results = {0};
    sctx::ResumeCounter c;
    int r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 0, "fresh resume status");
    CHECK(vm.resume_calls == 1 && vm.resume_error_calls == 0, "resume called once");
    CHECK(c.count == 0, "counter restored");
    CHECK(ctx.execution_time > 0.0, "outermost time accumulated");
}

static void testNestedResumeTimeAccounting() {
    sctx::ScriptContextState ctx;
    FakeVm vm;
    vm.resume_results = {0, 0};
    sctx::ResumeCounter outer, inner;
    // outer
    sctx::resumeGuard(ctx, vm, outer, 0, false, nullptr);
    double after_outer = ctx.execution_time;
    // inner (count starts at 1 because it happens *inside* the outer resume)
    inner.count = 1;
    sctx::resumeGuard(ctx, vm, inner, 0, false, nullptr);
    CHECK(ctx.execution_time == after_outer, "inner resume does not accumulate time");
    CHECK(vm.resume_calls == 2, "both resumes ran");
}

static void testDepthLimit() {
    sctx::ScriptContextState ctx;
    FakeVm vm;
    sctx::ResumeCounter c;
    c.count = sctx::kMaxResumeDepth + 1; // > 200
    int r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 2, "overflow with stack space reports (2)");
    CHECK(vm.pushed_errors.size() == 1 &&
          vm.pushed_errors[0] == "C stack overflow", "overflow message");
    CHECK(vm.resume_calls == 0, "no resume attempted");

    vm.pushed_errors.clear();
    vm.checkstack_ok = false;
    r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 5, "overflow without stack space is silent (5)");

    vm.pushed_errors.clear();
    vm.checkstack_ok = true;
    vm.push_ok = false;
    r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 5, "push failure is silent (5)");
    CHECK(c.count == sctx::kMaxResumeDepth + 1, "counter untouched on overflow");
}

static void testDeadline() {
    sctx::ScriptContextState ctx;
    ctx.shutdown_started = true;
    ctx.shutdown_deadline = 200.0;
    FakeVm vm;
    vm.clock = 199.0;
    sctx::ResumeCounter c;
    int r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 0, "at deadline-1 still runs");

    vm.clock = 200.0; // deadline == now is NOT past (binary: now != deadline)
    r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 0, "exactly at deadline still runs (strict compare)");

    vm.clock = 201.0;
    vm.resume_calls = 0; // reset from the successful cases above
    r = sctx::resumeGuard(ctx, vm, c, 0, false, nullptr);
    CHECK(r == 2, "past deadline reports (2)");
    CHECK(vm.pushed_errors.size() == 1 &&
          vm.pushed_errors[0] == "Not running script because past shutdown deadline",
          "deadline message");
    CHECK(vm.resume_calls == 0, "no resume past deadline");
    CHECK(c.count == 0, "counter never incremented");
}

static void testDeadlineFacetGate() {
    sctx::ScriptContextState ctx;
    ctx.shutdown_started = true;
    ctx.shutdown_deadline = 100.0;
    ctx.facet_phase = 3;
    FakeVm vm;
    vm.clock = 101.0;
    sctx::ResumeCounter c;
    CHECK_THROWS_MSG(sctx::resumeGuard(ctx, vm, c, 0, false, nullptr),
                     "Invalid Facet Access", "facet gate on deadline path");
}

static void testErrorResume() {
    sctx::ScriptContextState ctx;
    FakeVm vm;
    vm.resume_results = {6};
    std::string msg = "boom";
    sctx::ResumeCounter c;
    int r = sctx::resumeGuard(ctx, vm, c, 0, true, &msg);
    CHECK(r == 6, "error resume flows through");
    CHECK(vm.resume_error_calls == 1, "resumeerror called");
    CHECK(vm.pushed_errors.size() == 1 && vm.pushed_errors[0] == "boom",
          "error message pushed before resumeerror");

    vm.pushed_errors.clear();
    vm.resume_error_calls = 0;
    r = sctx::resumeGuard(ctx, vm, c, 0, true, nullptr);
    CHECK(r == 6 && vm.pushed_errors.empty(), "no message, no push");

    // checkstack failure on the error path is silent
    FakeVm vm2;
    vm2.checkstack_ok = false;
    sctx::ResumeCounter c2;
    r = sctx::resumeGuard(ctx, vm2, c2, 0, true, &msg);
    CHECK(r == 5, "error path without stack space is silent");
}

static void testOuterMapping() {
    CHECK(sctx::mapOuterResult(0).code == 0, "0 -> 0");
    CHECK(sctx::mapOuterResult(1).code == 1, "1 -> 1");
    auto six = sctx::mapOuterResult(6);
    CHECK(six.code == 1 && six.has_error_payload && six.payload_code == 6,
          "6 -> (1, payload 6)");
    for (int raw : {2, 4, 5}) {
        CHECK(sctx::mapOuterResult(raw).code == 2, "other -> 2");
    }
}

static void testFacetGate() {
    sctx::ScriptContextState ctx;
    ctx.facet_phase = 2;
    sctx::checkFacetPhase(ctx); // ok
    ctx.facet_phase = 3;
    CHECK_THROWS_MSG(sctx::checkFacetPhase(ctx), "Invalid Facet Access", "phase 3");
}

static void testVersionGate() {
    using sctx::ConfigLimits;
    // equal versions, zero limit → allowed
    CHECK(sctx::versionGate(10, {10, 0}), "same version, limit 0");
    // one ahead, limit 1 → allowed; two ahead, limit 1 → denied
    CHECK(sctx::versionGate(11, {10, 1}), "one ahead within limit");
    CHECK(!sctx::versionGate(12, {10, 1}), "two ahead beyond limit");
    // script behind config → u32 underflow → huge diff → denied
    CHECK(!sctx::versionGate(5, {10, 200}), "behind config denied (u32 wrap)");
}

static void testAdmission() {
    using sctx::AdmissionConfig, sctx::AdmissionRequest;
    AdmissionConfig cfg;
    cfg.cfgA = {100, 50};
    cfg.cfgB = {100, 50};

    // unrestricted container: no predicate → allow even RobloxScript class
    cfg.has_predicate = false;
    AdmissionRequest req;
    req.is_roblox_script_class = true;
    req.in_allowlist = false;
    req.schema_version = 100;
    CHECK(sctx::admissionAllowed(req, cfg), "no predicate allow-all");

    // restricted container: predicate present
    cfg.has_predicate = true;
    CHECK(!sctx::admissionAllowed(req, cfg), "RobloxScript class denied");
    req.is_roblox_script_class = false;
    CHECK(!sctx::admissionAllowed(req, cfg), "not in allowlist denied");
    req.in_allowlist = true;
    CHECK(sctx::admissionAllowed(req, cfg), "allowlisted allowed");

    // container mismatch denies before anything else
    req.owner_matches_context = false;
    CHECK(!sctx::admissionAllowed(req, cfg), "container mismatch denies");
    req.owner_matches_context = true;

    // version gates deny
    req.schema_version = 200; // (200-100)=100 > 50
    CHECK(!sctx::admissionAllowed(req, cfg), "version beyond limit denies");
}

static void testStartScriptFlow() {
    sctx::ScriptContextState ctx;
    sctx::StartScript ss(ctx);
    sctx::AdmissionRequest req;
    sctx::AdmissionConfig cfg;
    cfg.has_predicate = false;

    int load_calls = 0;
    auto load_ok = [&] { ++load_calls; return 0; };
    auto load_fail = [&] { return 7; };

    CHECK(sctx::StartOutcome::Started == ss.run(req, cfg, load_ok),
          "successful start");
    CHECK(load_calls == 1, "load step ran");
    CHECK(ctx.start_reentrancy == 0, "re-entrancy restored");

    CHECK(sctx::StartOutcome::LoadFailed == ss.run(req, cfg, load_fail),
          "load failure path");

    // denied request never reaches the load step
    load_calls = 0;
    sctx::AdmissionRequest denied = req;
    denied.owner_matches_context = false;
    CHECK(sctx::StartOutcome::Denied == ss.run(denied, cfg, load_ok),
          "denied before load");
    CHECK(load_calls == 0, "load step skipped on deny");

    // script-level refusal (vtable +0x320 false)
    sctx::AdmissionRequest refused = req;
    refused.script_level_run_ok = false;
    CHECK(sctx::StartOutcome::ScriptRefused == ss.run(refused, cfg, load_ok),
          "script refused");
}

static void testStartReentrancyFatal() {
    sctx::ScriptContextState ctx;
    ctx.start_reentrancy = 4; // > 3 → fatal on entry
    sctx::StartScript ss(ctx);
    sctx::AdmissionRequest req;
    sctx::AdmissionConfig cfg;
    cfg.has_predicate = false;
    CHECK_THROWS_MSG(ss.run(req, cfg, [] { return 0; }),
                     "startScript re-entrancy has exceeded 3", "re-entrancy fatal");

    // depth 3 works and restores the counter
    sctx::ScriptContextState ctx2;
    ctx2.start_reentrancy = 3;
    sctx::StartScript ss2(ctx2);
    CHECK(sctx::StartOutcome::Started ==
          ss2.run(req, cfg, [] { return 0; }), "depth-3 start ok");
    CHECK(ctx2.start_reentrancy == 3, "counter restored after start");
}

static void testModuleStateMachine() {
    using sctx::ModuleRecord, sctx::ModuleState, sctx::ModuleErrorKind;
    using sctx::RequireContextClass;

    sctx::ModuleRecord m;
    m.state = ModuleState::Ready;
    sctx::requireModule(m, false, RequireContextClass::Normal,
                        RequireContextClass::Normal, "mod"); // ok, no cycle
    CHECK_THROWS_MSG(sctx::requireModule(m, true, RequireContextClass::Normal,
                                         RequireContextClass::Normal, "mod"),
                     "use `export` instead of `return`", "cycle without export");

    sctx::ModuleRecord m2;
    m2.state = ModuleState::Ready;
    m2.export_based = true;
    sctx::requireModule(m2, true, RequireContextClass::Normal,
                        RequireContextClass::Normal, "mod"); // export cycle ok

    sctx::ModuleRecord m3;
    m3.state = ModuleState::Error;
    CHECK_THROWS_MSG(sctx::requireModule(m3, false, RequireContextClass::Normal,
                                         RequireContextClass::Normal, "mod"),
                     "experienced an error while loading", "error state");
    m3.error_kind = ModuleErrorKind::NotOneValue;
    CHECK_THROWS_MSG(sctx::requireModule(m3, false, RequireContextClass::Normal,
                                         RequireContextClass::Normal, "mod"),
                     "did not return exactly one value", "not-one-value");

    sctx::ModuleRecord m4;
    m4.state = ModuleState::Pending;
    m4.weak_ref_resolves = false;
    CHECK_THROWS_MSG(sctx::requireModule(m4, false, RequireContextClass::Normal,
                                         RequireContextClass::Normal, "mod"),
                     "destroyed and can no longer be required", "destroyed module");
    m4.weak_ref_resolves = true;
    sctx::requireModule(m4, false, RequireContextClass::Normal,
                        RequireContextClass::Normal, "mod"); // resolves ok

    sctx::ModuleRecord m5;
    m5.state = static_cast<ModuleState>(9);
    CHECK_THROWS_MSG(sctx::requireModule(m5, false, RequireContextClass::Normal,
                                         RequireContextClass::Normal, "mod"),
                     "Unknown module state", "unknown state");
}

static void testRequireContextRules() {
    using sctx::RequireContextClass;
    // .config cannot be required
    CHECK_THROWS_MSG(sctx::checkConfigModuleName(".config"),
                     ".config modules are configuration scripts", ".config rule");
    sctx::checkConfigModuleName("regular_module"); // ok

    // RobloxScript module required from normal context → fatal
    CHECK_THROWS_MSG(sctx::checkCrossContext(RequireContextClass::Normal,
                                             RequireContextClass::RobloxScript, "m"),
                     "RobloxScript module from a non RobloxScript context",
                     "cross-context normal->roblox");
    // normal module required from normal context → fine
    sctx::checkCrossContext(RequireContextClass::Normal,
                            RequireContextClass::Normal, "m");
    // a RobloxScript requirer skips both checks (the binary only gates
    // non-RobloxScript requirers)
    sctx::checkCrossContext(RequireContextClass::RobloxScript,
                            RequireContextClass::Normal, "m");
}

int main() {
    testFreshResume();
    testNestedResumeTimeAccounting();
    testDepthLimit();
    testDeadline();
    testDeadlineFacetGate();
    testErrorResume();
    testOuterMapping();
    testFacetGate();
    testVersionGate();
    testAdmission();
    testStartScriptFlow();
    testStartReentrancyFatal();
    testModuleStateMachine();
    testRequireContextRules();

    if (g_failures == 0) {
        std::printf("all scriptctx tests passed\n");
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
