# 03 — Networking + replication

Architecture from log channels, the string table, and analyzed dispatch
helpers. Code addresses for the inner replicator are still unmapped
(RBX symbols are visibility-hidden); the shape below is observed in
channels and strings unless noted.

## Layers, outside in

1. **Join**: protocol URL → `handleDeeplinkUrlV2` (`ram:0x1000376fd`) →
   `RBLinkingProtocol detectURL` + session via `0x1051e5369` →
   `GameJoinUtil` (50 refs), `JoinDataRxStats`, `AvatarJoinMetrics`.
2. **Transport**: `NetworkClient` over `RbxTransport`
   (`RnaExpConnection/Listener/Slot`, `TLS`, `QUIC`, `Ngtcp2`, `ChannelTracker`,
   `IoLibContext`, `DummyClient`), RakNet underneath
   (`RakNetConnectTrace/PktTrace`, `RaknetJoinOrDisconnectRequest`,
   `AddProfilerTelemetryScopeForRakNet`, ping breakdown
   `RDeserializeToPBQ`-style stages).
3. **Reception**: RakNet's receive loop (`RakNet::RecvFromLoop`,
   `ram:0x102d60715`) feeds a packet-return queue. The client-side
   `DeserializePacketsJob`/buffered-deserialize path is represented by
   `ram:0x10564eab9`: it checks the queue, removes records from a circular
   queue, updates receive-time counters, creates typed temporary records, and
   hands them to replication-side processing queues. It can stop or defer work
   when the per-step budget/queue state says so. The binary exposes the
   associated measurements `AvgTimeSpentInDeserializeQueue`,
   `CurrentNumPacketsWaitingForDeserialize`, and
   `deserializePacketsJob_total`.
4. **Apply**: `Network::ClusterSerializer::decodePBCChunk` (voxel
   `Grid_NEW` regions) + three replicators — `Large`, `Slim`, `Mega`
   (`LargeReplicatorTrace`, `MegaReplicatorLog/ConnData`,
   `ReplicatorPropertyCoalescing`, `SlimInstanceHashes`).
5. **Rules**: ownership/prediction gated on DataModel/Workspace membership
   (`Can only call Network Ownership API on a part that is descendent of
   Workspace`, `Cannot predict instance {}: Only instances under Workspace`,
   `NetworkOwnershipErrors`, `StreamingPinLog`, `IsSingleDataModelLifeCycleListener`).

## Player join/removal boundary

Player membership is represented as a replicated service operation, not only
as a local `Instance` insertion. The binary has separate network-side
`onPlayerAdded` and `onPlayerRemoving` paths with failure reporting, while the
Players service registers public lifecycle events. The supported lifecycle is:

```text
join/player replication item
  → create or resolve Player under Players
  → update player/session bookkeeping
  → fire PlayerAdded / connecting notifications
  → downstream services observe the new Player

leave/removal item
  → remove network/player bookkeeping
  → fire PlayerRemoving / disconnecting notifications
  → release dependent service references
```

The exact join opcode and wire fields remain unresolved. What is supported is
the boundary: APIs enforce that a Player used by client calls is a child of
`game.Players`, and service consumers receive lifecycle events after membership
changes.

## Voice is separate

`RealtimeMedia`, `StratusSession`, `WebrtcInput`, `LibWebrtcImpl`,
`CrossExperienceVoiceController`, `VoiceChat{Logs,ControlPlane}` (551 refs —
largest channel family), `RccRealtimeRelay`.

## Threading note

`0x102ee57de` (decompiled) is the cross-thread wake/dispatch helper used after
enqueue: thread primitives (`0x10320xxxx`), dispatch-to-main registration
(`"AppleMainThreadBlockingFix"`). Replication receive → deserialize → apply
crosses threads through the same job-queue machinery as scripting (doc 04).

## What is actually known about RakNet here

RakNet is a compiled transport layer, not the replication schema. The image
contains `RakPeer`, `RakPeerInterface`, `RakNetSocket`,
`CCRakNetSlidingWindow`, `BitStream`, and `RBX::Network::RakNetConnection`
types. Its receive loop handles socket/RakNet packet production, acknowledgements,
NAKs, split-packet reassembly, and packet-return ownership. Roblox's
`Replicator` then interprets the resulting application payload and produces
typed items such as `DeserializedNewObjectItem`,
`DeserializedChangePropertyItem`, `DeserializedDeleteInstanceItem`,
`DeserializedEventInvocationItem`, and join-data items.

The inspected deserializer has two important limits: the exact wire schema and
the helper type names are not recoverable from this build's hidden RBX symbols,
and the shown pseudocode is compiler output rather than source. Therefore the
C++ example models ownership and stage boundaries, not a claimed compatible
RakNet packet format.

## C++ sketch

`include/rbx/replicator.hpp` — `Transport → PacketQueue → Deserializer →
Replicator → DataModel apply` interfaces plus an ownership-gate example shaped
by the observed rules. No wire-format constants are claimed (not observed).
