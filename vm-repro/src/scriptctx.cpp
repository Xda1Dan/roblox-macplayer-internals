// scriptctx.cpp — implementation of the ScriptContext-layer state machines.
// Provenance for every rule: ../research/vm-research/scriptcontext.md and the
// header comment in include/rbxvm/scriptctx.hpp.
#include "rbxvm/scriptctx.hpp"

#include <cstdio>
#include <cstring>

namespace rbxvm {
namespace sctx {

namespace {

[[noreturn]] void fatal(const char* msg) {
    throw std::runtime_error(msg);
}

// The guard's return formula after pushing an error message:
//   (b ^ 1) + (b ^ 1) * 2 + 2  — b = push success (1) → 2, fail (0) → 5.
inline int reportOutcome(bool pushed) {
    return pushed ? RES_ERRREPORT : RES_ERRSILENT;
}

} // namespace

int resumeGuard(ScriptContextState& ctx, VmBoundary& vm, ResumeCounter& counter,
                int argcount, bool is_error, const std::string* errmsg,
                bool accumulate_time) {
    const int n = counter.count;

    if (n > kMaxResumeDepth) {
        // Only report when a slot is available for the message; otherwise the
        // failure is silent (5).
        if (vm.checkStack(1)) {
            if (vm.pushError(kMsgCStackOverflow, std::strlen(kMsgCStackOverflow)))
                return reportOutcome(true);
            return reportOutcome(false);
        }
        return RES_ERRSILENT;
    }

    counter.count = n + 1;

    int result;
    double t0 = 0.0;

    if (ctx.deadlinePassed(vm.now())) {
        // Past the shutdown deadline: facet access happens first (fatal at
        // phase >= 3), then the refusal error is reported if possible.
        checkFacetPhase(ctx);
        if (vm.checkStack(1)) {
            if (vm.pushError(kMsgPastShutdown, std::strlen(kMsgPastShutdown)))
                result = reportOutcome(true);
            else
                result = reportOutcome(false);
        } else {
            result = RES_ERRSILENT;
        }
        counter.count = n;
        return result;
    }

    const bool outermost = (counter.count == 1);
    if (outermost) {
        counter.t0 = vm.now();
        t0 = counter.t0;
    }

    if (!is_error) {
        result = vm.resume(argcount);
    } else {
        bool pushed = true;
        if (errmsg != nullptr) {
            if (!vm.checkStack(1)) {
                counter.count = n;
                return RES_ERRSILENT;
            }
            pushed = vm.pushError(errmsg->data(), errmsg->size());
            if (!pushed) {
                counter.count = n;
                return RES_ERRSILENT;
            }
        }
        result = vm.resumeError();
    }

    if (outermost && accumulate_time) {
        ctx.execution_time += vm.now() - t0;
    }

    counter.count = n;
    return result;
}

OuterResult mapOuterResult(int raw_status) {
    if (raw_status == RES_OK) return {0, false, 0};
    if (raw_status == RES_YIELD) return {1, false, 0};
    if (raw_status == RES_BREAK) return {1, true, RES_BREAK};
    return {2, false, 0}; // anything else (2, 4, 5, ...)
}

void checkFacetPhase(const ScriptContextState& ctx) {
    if (ctx.facet_phase >= kFacetPhaseLimit) fatal(kMsgInvalidFacet);
}

// ---------------------------------------------------------------------------
// startScript
// ---------------------------------------------------------------------------
bool versionGate(uint16_t script_version, const ConfigLimits& cfg) {
    // (uint)(u16)script - (uint)(u16)cfg.version > (uint)(u16)cfg.limit
    const uint32_t diff =
        static_cast<uint32_t>(script_version) - static_cast<uint32_t>(cfg.version);
    return diff <= static_cast<uint32_t>(cfg.limit);
}

bool restrictedPredicateBlocked(const AdmissionRequest& req, const AdmissionConfig& cfg) {
    // FUN_10144ef5e, exact control flow. versionGate() is the u32 expression
    // `(u16 script_ver - u16 cfg_ver) <= cfg_limit`.
    if (versionGate(req.schema_version, cfg.cfgA)) return false; // early allow

    if (req.script_present && versionGate(req.schema_version, cfg.cfgB) &&
        req.is_roblox_script_class)
        return false; // early allow

    return req.in_blocklist;
}

bool admissionAllowed(const AdmissionRequest& req, const AdmissionConfig& cfg) {
    // Step 2: container equality (sub_10159502e).
    if (!req.owner_matches_context) return false;

    // Step 3: capability dispatch (FUN_1015c4980). No predicate is
    // fail-closed: the helper returns 1, which the caller treats as blocked.
    if (!cfg.has_predicate) return false;
    if (restrictedPredicateBlocked(req, cfg)) return false;

    // sub_10159502e's second, flag-gated version window (DAT_10752c840):
    // only scripts with diffA > cfgA.limit pass; otherwise it logs the
    // "blocked from being ran under a restricted container" message.
    if (cfg.enforce_core_script_window && !versionGate(req.schema_version, cfg.cfgA))
        return false;
    return true;
}

StartOutcome StartScript::run(const AdmissionRequest& req, const AdmissionConfig& cfg,
                              const LoadStep& load_step) {
    // Step 2-3: policy gate.
    if (!admissionAllowed(req, cfg)) return StartOutcome::Denied;

    // Step 3 (startScript): script-level run check (vtable +0x320).
    if (!req.script_level_run_ok) return StartOutcome::ScriptRefused;

    // Step 4: re-entrancy counter (fatal beyond 3, restored on scope exit —
    // modeled with the saved-restore the binary performs).
    const uint32_t saved = ctx_.start_reentrancy;
    if (saved > kMaxStartReentrancy) fatal(kMsgReentrancy);
    ctx_.start_reentrancy = saved + 1;

    StartOutcome outcome;
    try {
        const int rc = load_step();
        if (rc == 0) {
            outcome = StartOutcome::Started; // → enqueue into ctx+0x208 queue
        } else {
            outcome = StartOutcome::LoadFailed; // → sub_101552b94 handler
        }
    } catch (...) {
        ctx_.start_reentrancy = saved;
        throw;
    }

    ctx_.start_reentrancy = saved;
    return outcome;
}

// ---------------------------------------------------------------------------
// require
// ---------------------------------------------------------------------------
void checkConfigModuleName(const std::string& name, bool enforce) {
    if (enforce && name == ".config") {
        // The binary formats the message with the module name; we keep the
        // literal template (the {} placeholder) exactly as stored.
        fatal(kMsgConfigNotRequireable);
    }
}

void checkCrossContext(bool skip_context_checks, RequireContextClass requirer,
                       RequireContextClass module_class,
                       const std::string& /*module_name*/) {
    // sub_10158dd88: FUN_101543e94()!=0 skips the pair; otherwise the
    // requirer's name-property bit 8 selects which direction is fatal.
    if (skip_context_checks) return;
    if (requirer == RequireContextClass::RobloxScript &&
        module_class != RequireContextClass::RobloxScript)
        fatal(kMsgCrossNormalFromRobloxScript);
    if (requirer != RequireContextClass::RobloxScript &&
        module_class == RequireContextClass::RobloxScript)
        fatal(kMsgCrossRobloxScriptFromNormal);
}

void requireModule(const ModuleRecord& module, bool module_in_cycle,
                   RequireContextClass requirer_class,
                   RequireContextClass module_class,
                   const std::string& module_name,
                   bool enforce_config_rule, bool skip_context_checks) {
    checkConfigModuleName(module_name, enforce_config_rule);
    checkCrossContext(skip_context_checks, requirer_class, module_class, module_name);

    switch (module.state) {
    case ModuleState::Ready:
        if (module_in_cycle && !module.export_based) fatal(kMsgRecursiveRequire);
        return; // value returned

    case ModuleState::Error:
        if (module.error_kind == ModuleErrorKind::NotOneValue)
            fatal(kMsgModuleNotOneValue);
        fatal(kMsgModuleLoadError);
        return;

    case ModuleState::Pending:
        if (!module.weak_ref_pending) return; // value already materialized
        if (!module.weak_ref_is_function) fatal(kMsgWeakRefNotFunction);
        if (module.weak_ref_returns_nil) fatal(kMsgModuleDestroyed);
        return; // weak ref resolves; value returned

    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "Unknown module state: %d",
                      static_cast<int>(module.state));
        throw std::runtime_error(buf);
    }
    }
}

} // namespace sctx
} // namespace rbxvm
