# TaskScheduler internals

> What this is: how *every* piece of async work — ObjC callbacks, network
> receives, Lua resumes, frame steps — becomes a refcounted job, lands in a
> sharded queue, and runs on a worker. Submit path is fully traced; worker
> dispatch is virtual.

```
caller (holds scheduler + payload of shared_ptrs)
  │  ram:0x10521311e  build 0x30-byte job (vtable 0x10742d0f0)
  ▼
ram:0x104f54dc6  copy + dispatch to queue index
  ▼
ram:0x104f54c96  lock shard mutex → copy item → ring insert → wake
  │  ├─ ram:0x104f54876 → ram:0x104545ba0  ring buffer growth/copy
  │  └─ ram:0x102ee57de  cross-thread wake (+ AppKit bounce)
  ▼
worker: arbiter → virtual step (+0x10) → virtual stop (+0x18)
  │  ram:0x102eec6da  JobStart wrapper
  │  ram:0x102eec76c  JobStop wrapper (timing + arbiter update)
  │  ram:0x102ee3136  bounded node allocator (exhaustion = fatal)
```

---

## 1. Enqueue — `ram:0x104f54c96`

**What it does:** the entire “submit a job” operation in 0xf7 bytes:
take the scheduler lock, pick the shard, copy the payload, append to the
ring, unlock, wake a worker, manage lifetimes.

**Function card**

| | |
|---|---|
| entry | `ram:0x104f54c96` |
| size | `0xf7` bytes, 71 instructions |
| locks | `+0x778` mutex (`0x10320983c` lock / `0x103209848` unlock) |
| queues | array at `+0x780`, stride `0x10` per index |
| covered here | lock, shard select, copy, insert, wake, release |

**Step by step (read against the assembly below)**

1. Prologue saves `R15/R14/R12/RBX`, reserves `0x20` — standard
   four-register working set: scheduler, item, index, temp.
2. `LEA RBX,[RDI+0x778]` — the shard mutex lives at a fixed offset.
   `CALL lock`. From here to the unlock, everything is under the lock.
3. `SHL RAX,0x4` then `LEA RSI,[R15+RAX]` + `ADD RSI,0x780` — shard
   selection is `base + index*0x10`. Not a hash; the *ring insert inside*
   does the modulo placement.
4. `CALL 0x10453826a` — copy the item into a stack temp. Payloads are
   copied under the lock, never referenced.
5. Null-check → `ADD RDI,0x1b0` → `CALL 0x104f54876` — the queue record
   at `+0x1b0` goes into the ring inserter.
6. `CALL unlock`. Then the wake: `CALL 0x102ee57de`.
7. Epilogue does *two* `XADD.LOCK [..+0x8],-1` sequences, each followed by
   `CALL [RAX+0x10]` + free on zero. That is `shared_ptr` release × 2 —
   the temp copies made at submit time.

**Assembly (excerpt, annotated — 71 insns total, showing the spine)**

```asm
104f54c96 PUSH RBP
104f54c97 MOV RBP,RSP
104f54c9a PUSH R15
104f54c9c PUSH R14
104f54c9e PUSH R12
104f54ca0 PUSH RBX
104f54ca1 SUB RSP,0x20
104f54ca5 MOV R12D,EDX          ; queue index
104f54ca8 MOV R14,RSI           ; item
104f54cab MOV R15,RDI           ; scheduler
104f54cb5 LEA RBX,[RDI+0x778]   ; shard mutex
104f54cc7 CALL 0x10320983c      ; lock
104f54ccc MOVSXD RAX,R12D
104f54ccf SHL RAX,0x4           ; index * 0x10
104f54cd3 LEA RSI,[R15+RAX]
104f54cd7 ADD RSI,0x780         ; queue slot
104f54ce5 CALL 0x10453826a      ; copy item -> stack temp
104f54cea MOV RDI,[R15]         ; queue record (checked)
104f54cf0 JZ  0x104f54d01       ; empty -> skip insert
104f54cf2 ADD RDI,0x1b0
104f54cfc CALL 0x104f54876      ; ring insert -> 0x104545ba0
104f54d01 MOV RDI,RBX
104f54d04 CALL 0x103209848      ; unlock
104f54d23 CALL 0x102ee57de      ; wake workers
; ... then two shared_ptr releases:
104f54d38 XADD.LOCK [RBX+0x8],RAX(-1)
104f54d46 CALL [RAX+0x10]       ; destroy on last ref
```

**Simplified reconstruction**

```cpp
// Original illustration of the observed discipline.
void enqueue(Scheduler* s, JobItem item, uint32_t index) {
  std::unique_lock lock(s->shardMutex());       // +0x778
  QueueSlot& q = s->slot(index);                // +0x780 + index*0x10
  JobItem tmp = copy(item);                     // 0x10453826a
  if (q.record) ringInsert(q.record + 0x1b0, tmp);
  lock.unlock();                                // scope exit
  wakeWorkers(std::move(tmp));                  // 0x102ee57de
} // two shared_ptr releases on temp copies
```

---

## 2. Ring buffer — `ram:0x104545ba0`

**What it does:** grows/copies the 0x80-byte record ring. Capacity grows
`cap*3/2` (min 4); placement is `((count + head) % capacity) * 0x80`.

**Correction note:** an earlier revision called this an open-addressing
hash table. It is not — re-reading the growth/copy body shows a circular
buffer with head-offset rehoming. The queue *index* selects the shard; the
*ring math* places the record.

```cpp
// Shape only.
void ringAppend(Ring& r, const Record* first, const Record* last) {
  size_t n = last - first;
  if (r.count + n > r.capacity) {
    size_t cap = r.capacity * 3 / 2;
    if (cap < 4) cap = 4;
    regrow(r, cap);                             // via 0x104545c64
  }
  for (auto* p = first; p != last; ++p) {
    size_t slot = (r.head + r.count++) % r.capacity;
    memcpy(&r.base[slot * 0x80], p, 0x80);
  }
}
```

---

## 3. Run — virtual step/stop + node pool

**`ram:0x102eec6da` (JobStart)** — mark active, log arbiter/timestamp,
`CALL [job+0x10]`. The scheduler never switches on job type; dispatch is
one indirect call.

**`ram:0x102eec76c` (JobStop)** — clear active, `CALL [job+0x18]`,
record elapsed/stop time, update arbiter, emit `JobStop` (+ per-thread
diagnostic ring under profiling flags).

**`ram:0x102ee3136` (node allocator)** — pull a node from the bounded
pool, copy shared state with atomic incs, CAS-publish into the arbiter
table. Exhaustion is fatal:

```text
Out of job nodes. Check for job leaks and/or increase the
FInt::TaskSchedulerMaxNumOfJobs value
```

```cpp
// Observed lifecycle, simplified.
JobNode* n = pool.acquire();   // bounded; fatal if empty
n->job = sharedJob;            // atomic ref bumps
n->deadline = job->deadline();
arbiter.publish(n);            // atomic
worker: n->active = true; n->job->step(stats); // [job+0x10]
worker: n->job->stop(); n->active = false;     // [job+0x18]
pool.release(n);
```

---

## Patterns to recognize everywhere

| Bytes | Meaning |
|---|---|
| `INC.LOCK [r+0x8]` | `shared_ptr` copy (ref bump) |
| `XADD.LOCK [r+0x8],-1` + `JZ` → `CALL [RAX+0x10]` | `shared_ptr` release; destroy on last |
| `LEA RBX,[RDI+0x778]` + paired calls | shard-mutex lock scope |
| `SHL …,0x4` + `ADD …,0x780` | shard slot select |
| stack canary + `___stack_chk_fail` | every ObjC entry + engine fn above |

## Unknowns

- Worker-thread entry / steal policy between shards.
- Exact arbiter priority formula (deadline + flags observed, weights not).
- Per-cycle task budgets per job class (tunables exist, values build-specific).
