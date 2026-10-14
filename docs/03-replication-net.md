# Networking reference

In one paragraph: RakNet moves bytes. A receive loop owns the socket. A
buffered deserializer drains queued packets on a time budget. A
replicator turns payloads into typed items. The DataModel applies them
only when ownership and location rules pass. Voice uses a separate media
stack.

## Layers, outside in

1. **Join** — protocol URL, `handleDeeplinkUrlV2` (`0x1000376fd`),
   session via `0x1051e5369`, plus join stats and avatar metrics.
2. **Transport** — `NetworkClient` over `RbxTransport` (connections,
   listeners, slots, TLS, QUIC, channel tracking), RakNet underneath
   (connect and packet traces, join-or-disconnect requests, ping stages).
3. **Receive** — `RakNet::RecvFromLoop` (`0x102d606da`) feeds a
   packet-return queue. The buffered deserializer (`0x10564dd3c`)
   drains it, updates receive timing and queue counters, builds typed
   temporary records, and hands them to replication processing. Work can
   stop or defer on budget. Related counters: `AvgTimeSpentInDeserializeQueue`,
   `CurrentNumPacketsWaitingForDeserialize`, `deserializePacketsJob_total`.
4. **Apply** — voxel chunk decoding plus `Large`, `Slim`, and `Mega`
   replicators with property coalescing and instance hashing.
5. **Rules** — ownership and prediction need DataModel and Workspace
   membership. Examples: network-ownership calls require a `Workspace`
   descendant; prediction requires an instance under `Workspace`.

Step by step for one batch:

```text
socket bytes
  -> RakNet framing, acks, split reassembly
  -> packet-return queue
  -> DeserializePacketsJob slice
  -> typed item (new object, property change, delete, event call, join data)
  -> ownership and location check
  -> DataModel mutation or drop with a diagnostic
```

## Player join and removal

Membership is a replicated operation, not only a local insert.

```text
join item
  -> create or resolve Player under Players
  -> update player and session bookkeeping
  -> fire PlayerAdded and connecting notifications
  -> downstream services observe the new Player

removal item
  -> remove network and player bookkeeping
  -> fire PlayerRemoving and disconnecting notifications
  -> release dependent references
```

Join opcodes and wire fields are not claimed here. The boundary is: client
calls require a `Player` under `game.Players`, and consumers hear
lifecycle events after membership changes.

## Voice

Voice is separate: realtime media, Stratus sessions, WebRTC input and
implementation, cross-experience voice control, and voice-chat logs and
control plane.

## Threading

After enqueue, the cross-thread wake helper at `0x102ee57de` moves work to
workers and can bounce UI work to the main thread
(`AppleMainThreadBlockingFix`). Receive, deserialize, and apply cross
threads through the same job machinery as scripting.

## RakNet in this build

RakNet is the transport, not the replication schema. Present types include
`RakPeer`, `RakPeerInterface`, `RakNetSocket`,
`CCRakNetSlidingWindow`, `BitStream`, and `RBX::Network::RakNetConnection`.

The receive loop handles socket production, acknowledgements, negative
acknowledgements, split-packet reassembly, and packet-return ownership.
The replicator then produces items such as
`DeserializedNewObjectItem`, `DeserializedChangePropertyItem`,
`DeserializedDeleteInstanceItem`, `DeserializedEventInvocationItem`, and
join-data items.

Behavior in code form:

```cpp
// Simplified illustration.
void onPacketReceived(Packet packet) {
  auto changes = decode(packet);  // bytes -> typed items
  applyToDataModel(changes);      // gated by ownership and location
}

bool canApply(const Instance& target) {
  // Only Workspace members replicate and predict.
  return target.isDescendantOf(workspace());
}
```

No wire-format constants are claimed. The C++ model covers ownership and
stage boundaries only. See `include/rbx/replicator.hpp`.

## Addresses in this build

| Place | Address |
|---|---|
| Deeplink handler | `0x1000376fd` |
| Session accessor | `0x1051e5369` |
| Receive loop | `0x102d606da` |
| Buffered deserializer | `0x10564dd3c` |
| Cross-thread wake | `0x102ee57de` |

## What is not claimed

- Exact packet opcodes and field codecs.
- Helper names behind internal deserialize calls.
- Tunable budget values for other builds.
