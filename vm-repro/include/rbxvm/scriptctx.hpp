// scriptctx.hpp — C++ reproduction of the ScriptContext layer of
// RobloxPlayer (macOS) build f9247f9560044102 / Luau 0.742.
//
// Everything here mirrors verified behavior of:
//   - the resume guard   FUN_101556cb4 @ VA 0x101556cb4
//   - ScriptContextResume sub_1015566d6 @ VA 0x1015566d6 (result mapping)
//   - the facet gates    FUN_101556664 / FUN_1040d49da
//   - startScript        FUN_10158afe8 (re-entrancy counter + admission chain)
//   - the admission predicate  sub_10159502e / FUN_1015c4980 / FUN_10144ef5e
//   - require            sub_10158dd88 (module state machine)
//
// The real VM boundary (lua_resume / lua_resumeerror / luaD_checkstack / error
// push) is injected behind `VmBoundary` so the state machines can be exercised
// and tested without a Lua state. Fatal error messages are reproduced verbatim
// from the binary's string constants.
//
// Full analysis: ../research/vm-research/scriptcontext.md.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace rbxvm {
namespace sctx {

// ---------------------------------------------------------------------------
// Constants verified in the binary
// ---------------------------------------------------------------------------
constexpr int kMaxResumeDepth = 200;      // the guard's C-stack/re-entrancy limit
constexpr long kMaxStackSlots = 8001;     // luaD_checkstack limit (0x1f41 TValues)
constexpr uint32_t kMaxStartReentrancy = 3; // startScript nesting limit
constexpr int kFacetPhaseLimit = 3;       // facet gates fatal at phase >= 3

// Raw resume statuses that flow through the guard (Luau thread statuses as
// observed in this build's result handling).
enum RawStatus : int {
    RES_OK = 0,          // LUA_OK   — finished
    RES_YIELD = 1,       // LUA_YIELD
    RES_ERRREPORT = 2,   // error, message pushed on the stack
    RES_ERRSILENT = 5,   // error, nothing to report (checkstack/push failed)
    RES_BREAK = 6,       // LUA_BREAK / the wrapper's "error with payload"
};

// Fatal messages reproduced verbatim from the binary.
constexpr const char* kMsgCStackOverflow = "C stack overflow";
constexpr const char* kMsgPastShutdown = "Not running script because past shutdown deadline";
constexpr const char* kMsgReentrancy = "startScript re-entrancy has exceeded 3";
constexpr const char* kMsgInvalidFacet = "Invalid Facet Access";
constexpr const char* kMsgRecursiveRequire =
    "Requested module was required recursively. To allow circular dependencies, "
    "all modules in the cycle must use `export` instead of `return`";
constexpr const char* kMsgModuleLoadError = "Requested module experienced an error while loading";
constexpr const char* kMsgModuleNotOneValue = "Module code did not return exactly one value";
constexpr const char* kMsgModuleDestroyed =
    "Requested module has been destroyed and can no longer be required";
constexpr const char* kMsgWeakRefNotFunction =
    "Weak module reference is expected to resolve to a table";
constexpr const char* kMsgUnknownModuleState = "Unknown module state: %d";
constexpr const char* kMsgConfigNotRequireable =
    "cannot require '{}': .config modules are configuration scripts and cannot be required directly";
constexpr const char* kMsgCrossRobloxScriptFromNormal =
    "Cannot require a RobloxScript module from a non RobloxScript context";
constexpr const char* kMsgCrossNormalFromRobloxScript =
    "Cannot require a non-RobloxScript module from a RobloxScript";
constexpr const char* kMsgRestrictedContainer =
    "Script \"{}\" was blocked from being ran under a restricted container.";
constexpr const char* kMsgBadCacheType = "Unexpected type in script compilation cache.";

// ---------------------------------------------------------------------------
// ScriptContext state (verified offsets noted in comments; ctx-relative)
// ---------------------------------------------------------------------------
struct ScriptContextState {
    uint32_t start_reentrancy = 0;   // ctx+0x38 (u32)
    double execution_time = 0.0;     // ctx+0x28 (double, seconds accumulated
                                     // across outermost resumes)
    double shutdown_deadline = 0.0;  // sub-object ctx+0x4b8, field +0x38
    bool shutdown_started = false;   // sub-object ctx+0x4b8, +0x40 bit0
    int facet_phase = 0;             // ctx+0xa18 (fatal at >= 3)

    // FUN_1015c486e: past the deadline iff shutdown started and now > deadline
    // (the binary tests deadline <= now && now != deadline, i.e. strictly >).
    bool deadlinePassed(double now) const {
        return shutdown_started && shutdown_deadline <= now && now != shutdown_deadline;
    }
};

// The guard-counter struct carried by each resume task (task slot [0]):
// {int32 count; double t0}.
struct ResumeCounter {
    int count = 0;
    double t0 = 0.0;
};

// ---------------------------------------------------------------------------
// VM boundary — the injected equivalent of lua_resume / lua_resumeerror /
// luaD_checkstack / error push / clock.
// ---------------------------------------------------------------------------
class VmBoundary {
public:
    virtual ~VmBoundary() = default;
    virtual int resume(int /*argcount*/) = 0;             // lua_resume(L, 0, n)
    virtual int resumeError() = 0;                        // lua_resumeerror(L, 0)
    virtual bool checkStack(int /*n*/) = 0;               // luaD_checkstack(L, n)
    virtual bool pushError(const char* /*msg*/, size_t /*len*/) = 0;
    virtual double now() = 0;
};

// ---------------------------------------------------------------------------
// The resume guard (FUN_101556cb4), faithful to the decompiled order:
//   depth check -> deadline check (with facet access) -> resume/resumeError
//   -> outermost time accounting -> counter restore.
// Returns a RawStatus. `accumulate_time` models the profiler flag read at
// *(*(L->userdata…)+0x228) == 0 (true = account time into ctx.execution_time).
// ---------------------------------------------------------------------------
int resumeGuard(ScriptContextState& ctx, VmBoundary& vm, ResumeCounter& counter,
                int argcount, bool is_error, const std::string* errmsg,
                bool accumulate_time = true);

// The wrapper-level result mapping of sub_101556235 / sub_1015562c1:
//   0 -> 0 (done) | 1 -> 1 (yield) | 6 -> (1, payload code 6) | other -> 2.
struct OuterResult {
    int code;                // what the scheduler sees
    bool has_error_payload;  // result struct carries the error info
    int payload_code;        // raw status when has_error_payload
};
OuterResult mapOuterResult(int raw_status);

// ---------------------------------------------------------------------------
// Facet gates (FUN_101556664 / FUN_1040d49da) — valid while phase < 3.
// Throws std::runtime_error(kMsgInvalidFacet) at phase >= 3.
// ---------------------------------------------------------------------------
void checkFacetPhase(const ScriptContextState& ctx);

// ---------------------------------------------------------------------------
// startScript model (FUN_10158afe8 + sub_10159502e + FUN_1015c4980 +
// FUN_10144ef5e). Only the verified decision logic is modeled; the object
// bookkeeping (records, sets, refcounts) is represented by outcomes.
//
// Polarity (verified from sub_10159502e's disassembly):
//   FUN_1015c4980 / FUN_10144ef5e return 0 = allowed, 1 = blocked.
//   With no predicate installed, FUN_1015c4980 returns 1 (fail-closed).
//   The set at predicate+0x1a8 is a BLOCKED/malicious set, not an allowlist:
//   a hit returns 1 and the caller logs "detected as malicious".
// ---------------------------------------------------------------------------
enum class RunContext { Normal = 0, Shared = 3 }; // script+0x110 enum value 3 re-resolves

struct ConfigLimits { // the two config singletons (u16 version at +0x1b8, limit +0x1ba)
    uint16_t version = 0;
    uint16_t limit = 0;
};

struct AdmissionRequest {
    RunContext run_context = RunContext::Normal;
    bool owner_matches_context = true;   // step 2: container equality
    bool script_level_run_ok = true;     // script vtable +0x320 bool
    bool script_present = true;          // request->script != null (gate B is skipped if null)
    bool is_roblox_script_class = false; // FUN_1014d24d4
    bool in_blocklist = false;           // predicate+0x1a8 membership (1 = blocked)
    uint16_t schema_version = 0;         // script+0x18 → +0x1b8
};

struct AdmissionConfig {
    bool has_predicate = true;           // ctx+0x488 +0x60 != 0 (false = fail-closed)
    bool enforce_core_script_window = true; // DAT_10752c840: second version window
    ConfigLimits cfgA{};                 // FUN_1004c9be8 (CoreScript config)
    ConfigLimits cfgB{};                 // FUN_100537686 (ModuleScript config)
};

// The version window predicate used by FUN_10144ef5e: true iff
// (script_version - cfg.version) <= cfg.limit, all in u32 arithmetic.
bool versionGate(uint16_t script_version, const ConfigLimits& cfg);

// FUN_10144ef5e: true = blocked. Exact control flow:
//   diffA = script_ver - cfgA.version
//   if (diffA <= cfgA.limit) return false;                 // early allow
//   if (script_present) {
//       diffB = script_ver - cfgB.version
//       if (diffB <= cfgB.limit && is_roblox_script_class) return false;
//   }
//   return in_blocklist;
bool restrictedPredicateBlocked(const AdmissionRequest& req, const AdmissionConfig& cfg);

// The full admission chain; false = blocked. Mirrors sub_10159502e:
//   owner mismatch -> blocked; predicate result -> blocked; then the optional
//   CoreScript version window (DAT_10752c840) can block an otherwise allowed
//   script when diffA > cfgA.limit.
bool admissionAllowed(const AdmissionRequest& req, const AdmissionConfig& cfg);

// startScript control flow: re-entrancy accounting + load-step dispatch.
// `load_step` models sub_101593650: return 0 = loaded OK, non-zero = failure.
enum class StartOutcome { Started, Denied, ScriptRefused, LoadFailed };
class StartScript {
public:
    explicit StartScript(ScriptContextState& ctx) : ctx_(ctx) {}

    using LoadStep = std::function<int()>; // 0 = ok, non-zero = error code

    StartOutcome run(const AdmissionRequest& req, const AdmissionConfig& cfg,
                     const LoadStep& load_step);

    uint32_t currentDepth() const { return ctx_.start_reentrancy; }

private:
    ScriptContextState& ctx_;
};

// ---------------------------------------------------------------------------
// require / module state machine (sub_10158dd88)
// ---------------------------------------------------------------------------
enum class ModuleState : int { Ready = 1, Error = 2, Pending = 3 };
enum class ModuleErrorKind : int { LoadError = 0, NotOneValue = 1 };

struct ModuleRecord {
    ModuleState state = ModuleState::Pending;
    bool export_based = false;                        // FUN_1014d190e
    // Pending-state weak-reference resolution (FUN_1014d18fa / FUN_102985ba1):
    // state == -1 means "resolve now"; the target must be a function (tag 7),
    // and a nil result means the module was destroyed.
    bool weak_ref_pending = true;                     // FUN_1014d18fa(state) == -1
    bool weak_ref_is_function = true;
    bool weak_ref_returns_nil = false;
    ModuleErrorKind error_kind = ModuleErrorKind::LoadError;
};

enum class RequireContextClass { Normal, RobloxScript };

// Throws std::runtime_error with the binary's exact message on every fatal
// path; returns normally when the require succeeds.
// `enforce_config_rule` models the DAT_10752c0a0 gate around the ".config"
// rejection; `skip_context_checks` models FUN_101543e94() != 0.
void requireModule(const ModuleRecord& module, bool module_in_cycle,
                   RequireContextClass requirer_class,
                   RequireContextClass module_class,
                   const std::string& module_name,
                   bool enforce_config_rule = true,
                   bool skip_context_checks = false);

// Precheck helpers (the entry-path rules), individually testable:
void checkConfigModuleName(const std::string& name, bool enforce = true); // ".config" rule
// FUN_1016fb2ae(name, 8) derives the requirer's RobloxScript flag from a
// name-property bitset; FUN_101543e94()!=0 skips both checks. The binary
// checks both directions (the old model only had one).
void checkCrossContext(bool skip_context_checks, RequireContextClass requirer,
                       RequireContextClass module, const std::string& module_name);

} // namespace sctx
} // namespace rbxvm
