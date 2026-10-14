# Replication deserialize path

> What this is: how received network bytes become typed replication work
> without ever blocking the receive thread. One big job drains queues,
> budgets time, builds temp records, and hands them to the replicator.

```
RakNet::RecvFromLoop  0x102d606da
  |  socket -> RakNet -> packet-return queue (split/ack/NAK handled inside RakNet)
  v
Replicator::OnReceive  0x10561fb20
  |  read message-ID byte -> dispatch table below
  |  0x15/0x16 connection, 0x1b physics, 0x81/0x83/0x86 replication
  |  typed items via 0x1055fa738 (returns 2 = queued, 1 = inline, 0 = special)
  v
buffered deserializer  0x10564dd3c  (0xde0 bytes, ~0xb98 frame)
  |  lock queue -> pop record -> timing/counters -> temp record
  |  -> replication queue -> budget check -> repeat or stop
  v
Replicator receive (OnReceive / V2 paths)
  |  typed Deserialized*Item (NewObject, ChangeProperty, Delete,
  |  EventInvocation, join-data, stream, physics, ...)
  v
DataModel apply (ownership / Workspace-membership gated)
```

---

## 1. Entry - 0x10564dd3c

**What it does:** drain loop for the deserialize job. Sets up a large
scratch frame, locks the receive queue, snapshots queue cursors, then
loops: pop one record, account time, build a typed temp, hand off, check
budget/deadline.

**Function card**

| | |
|---|---|
| entry | 0x10564dd3c |
| size | 0xde0 bytes |
| frame | `SUB RSP,0xb98` - large scratch for temp records + queue cursors |
| locks | queue mutex at `RBX+0x188` (`0x10320983c` / unlock on exit paths) |
| covered here | prologue, queue lock, cursors, drain loop head |

**Step by step (prologue in order)**

1. Standard prologue plus stack canary (`[0x106e65040]` -> `[RBP-0x30]`).
   This function can fail safely; the canary is checked on every exit.
2. Zero a temp record with `PXOR XMM0,XMM0` + `MOVDQA` stores. Fresh temp
   means no stale bytes leak from one packet to the next.
3. Flag-gated early path, then `TEST [RBX+0x180]+0x4948` - a mode/feature
   switch that selects the drain variant.
4. Timestamp snapshot (`CALL 0x102f7eb5e` -> `[RBX+0x398]`). All later
   budget math is relative to this.
5. `LEA R15,[RBX+0x188]` + `CALL lock` - the receive queue is now owned.
6. Weak/shared resolve (`0x104f57bde` / `0x104f58ad4`) then queue-shape
   setup (`0x10564ed14`). The `CMP [RBP-0x1a8],1` branch selects temp
   cleanup behavior.
7. Optional notify hook at `[RAX+0x54d8]` (`CALL [RAX]` if non-null).
8. Spill ~10 queue cursors (`RBX+0x1d0/0x218/0x228/0x268/0x288/0x290/
   0x1b0/0x210/0x368`...) into stack slots. The loop below walks these,
   not the object directly - classic “snapshot cursors under lock, then
   iterate” shape.
9. `CALL 0x1016e9386` - queue-empty check. Empty plus secondary-queue
   empty means jump to finish/time-accounting; otherwise enter the pop
   branch selected by flag `0x10789d2c8`.

**Assembly - prologue spine (excerpt)**

```asm
10564dd3c PUSH RBP
10564dd49 SUB RSP,0xb98        ; scratch frame
10564dd50 MOV RBX,RDI           ; job/object
10564dd5d MOV [RBP-0x30],RAX   ; stack canary
10564dd76 PXOR XMM0,XMM0
10564dd81 MOVDQA [RDX-0x78],XMM0 ; zero temp record
10564dd93 MOV RAX,-0x1
10564dd9a MOV [RDX-0x50],RAX
10564ddc7 CALL 0x102e9815d
10564ddcc MOV RAX,[RBX+0x180]
10564ddd3 MOV EAX,[RAX+0x4948]  ; mode switch
10564de3a CALL 0x102f7eb5e      ; now
10564de3f MOVQ [RBX+0x398],XMM0 ; t_start
10564de47 LEA R15,[RBX+0x188]   ; queue lock
10564de5c MOV RDI,R15
10564de5f CALL 0x10320983c      ; lock
10564de72 MOV RDI,[RBX+0x180]
10564de79 CALL 0x104f57bde      ; shared resolve
10564de88 CALL 0x104f58ad4
10564de9b CALL 0x10564ed14      ; queue-shape setup
10564df04 LEA RAX,[RBX+0x288]   ; cursor snapshots...
10564df0c LEA RAX,[RBX+0x290]
10564df1a LEA RAX,[RBX+0x1b0]
10564df74 MOV RDI,R13
10564df77 CALL 0x1016e9386      ; empty?
```

**Simplified reconstruction**

```cpp
// Original illustration.
void deserializeDrain(Job* j) {
  ScopedCanary guard;
  TempRecord tmp = {};                    // PXOR-zeroed
  if (!modeEnabled(j)) return finish(j);
  j->tStart = now();
  std::unique_lock lock(j->queueMutex()); // RBX+0x188
  auto shared = j->sharedCtx();           // weak->shared
  QueueView v = setupView(j, tmp);
  if (v.notify) v.notify();
  Cursors c = snapshot(j);                // RBX+offsets -> stack
  while (true) {
    if (queueEmpty(c)) {
      if (secondaryEmpty(c)) break;       // -> time accounting
      popSecondary(c, tmp);
    } else {
      popPrimary(c, tmp);
    }
    accountTime(j, tmp);
    handToReplicator(tmp);
    if (overBudget(j)) break;             // defer remainder
  }
  finish(j);                              // timing + unlock + release
}
```

---

## 2. What the loop guarantees

- Receive thread never applies: it only produces queue records.
- Deserialize never blocks on the network: pop, convert, hand off, repeat.
- Time-bounded: per-step budget can stop the drain early; the remainder
  waits for the next job slice (hence `AvgTimeSpentInDeserializeQueue`,
  `CurrentNumPacketsWaitingForDeserialize`).
- Ownership is explicit: temp records are stack-scoped, shared state uses
  atomic ref bumps, queue cursors are snapshots.

## 2. OnReceive dispatch — `0x10561fb20`

**What it does:** reads the first payload byte and routes the packet.
Return code is the contract: `0` = special-cased, `1` = handled inline,
`2` = queued as a typed item for the deserializer.

**Step by step**

1. Read one byte from the bitstream (the message ID) plus a telemetry
   classifier: `0x83→0x1f`, `0x86→2`, `0x8d→0xf`, `0xa1→1`, `0xa5→0x1e`,
   `0xa6→3`. Unknown IDs fall through to the default path.
2. Connection bytes `0x15` / `0x16`: log `Lost connection` /
   `ID_DISCONNECTION_NOTIFICATION`, tear down the replicator session,
   report the disconnect reason.
3. Physics byte `0x1b`: read the replication fields, ask the physics
   receiver (`+0x408` virtual) whether it is ready — unready means the
   packet is dropped with `call to onreceive before physicsReceiver is
   initialized`. Otherwise enforce the basis-index high-water mark
   (out-of-order bases are reconciled, not applied blindly), apply, then
   copy the record (`0x10563c474`) and hand it to the item queue
   (`0x1055fa738`). Returns `2`.
4. Replication bytes `0x81`, `0x83`, `0x86`: copy the bitstream words,
   copy the record, hand to the item queue. Returns `2`. Bytes `0x82`,
   `0x89`, `0x91` are ignored. Byte `0x84` takes a special path
   (allocate `0x48`, `0x1055cf5b0`, publish at `+0x71b`). Bytes `0x87`,
   `0x88`, `0x8b`, `0x8c` are flag-gated.
5. Everything else lands in the default path (`LAB_105620d49`): generic
   replication-data handling, returns `1`.

The handoff `0x1055fa738(replicator, &item)` is the single funnel that
creates the typed `Deserialized*Item` queue entries (new object, property
change, delete, event invocation, join data). Every `returns 2` path ends
there; that is why the deserializer at `0x10564dd3c` only ever sees
typed records, never raw sockets.

## Unknowns

- Exact wire opcodes and field codecs (not recovered; hidden symbols).
- Full callee names behind the `0x1056xxxx` / `0x1016xxxx` helpers.
- Budget/deadline tunable values for this build.
