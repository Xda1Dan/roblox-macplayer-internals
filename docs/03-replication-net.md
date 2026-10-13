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
3. **Reception**: `DeserializePacketsJob` batches on the scheduler
   (`deserializePacketsJob_total`, `AvgTimeSpentInDeserializeQueue`,
   `CurrentNumPacketsWaitingForDeserialize`, `ReplicationJoinBatchLog`,
   `ReplicationDataCaching/Lifetime`, `SlimReplicationLog`).
4. **Apply**: `Network::ClusterSerializer::decodePBCChunk` (voxel
   `Grid_NEW` regions) + three replicators — `Large`, `Slim`, `Mega`
   (`LargeReplicatorTrace`, `MegaReplicatorLog/ConnData`,
   `ReplicatorPropertyCoalescing`, `SlimInstanceHashes`).
5. **Rules**: ownership/prediction gated on DataModel/Workspace membership
   (`Can only call Network Ownership API on a part that is descendent of
   Workspace`, `Cannot predict instance {}: Only instances under Workspace`,
   `NetworkOwnershipErrors`, `StreamingPinLog`, `IsSingleDataModelLifeCycleListener`).

## Voice is separate

`RealtimeMedia`, `StratusSession`, `WebrtcInput`, `LibWebrtcImpl`,
`CrossExperienceVoiceController`, `VoiceChat{Logs,ControlPlane}` (551 refs —
largest channel family), `RccRealtimeRelay`.

## Threading note

`0x102ee57de` (decompiled) is the cross-thread wake/dispatch helper used after
enqueue: thread primitives (`0x10320xxxx`), dispatch-to-main registration
(`"AppleMainThreadBlockingFix"`). Replication receive → deserialize → apply
crosses threads through the same job-queue machinery as scripting (doc 04).

## C++ sketch

`include/rbx/replicator.hpp` — `Transport → PacketQueue → Deserializer →
Replicator → DataModel apply` interfaces plus an ownership-gate example shaped
by the observed rules. No wire-format constants are claimed (not observed).
