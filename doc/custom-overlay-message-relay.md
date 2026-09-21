# External message relay from custom overlays

Messages received from an authorized custom-overlay sender are checked by the existing
external-message admission pipeline. Once admission and broadcast permission succeed,
the node sends the message to its public shard overlay. This path does not send the
message back into custom overlays and does not affect messages received from public overlays.
The global external-message broadcast disable flag is respected.

A node suppresses repeat custom-to-public relays of the same BOC for 60 seconds, using
an actor-local LRU cache of at most 8192 entries. Duplicate receipts do not extend the
expiry. Public overlay deduplication remains in place. Acceptance or broadcast does not
prove inclusion in a block.

For a bounded operational check, set `DTON_TRACE_EXT_MESSAGE_RELAY=1` on the node.
Trace records contain the SHA-256 of the serialized BOC (not the cell root hash):

- `custom.recv`: authorized custom overlay message received, before admission.
- `public.enqueue`: admission succeeded and the public shard send was queued.
- `public.broadcast`: the public shard reached the overlay broadcast call.

Trace contains no BOC payload. Match all three stages by `boc_hash` to distinguish
relay from an unrelated direct LiteServer request. A missing later stage can indicate
admission rejection, disabled broadcasting, an unavailable shard, or deduplication.
The final stage proves a broadcast request, not receipt by a validator or inclusion.

Build and run `test-external-message-relay` to check duplicate suppression, expiry,
and bounded-cache eviction. Build `validator-engine` to validate the actor integration.
