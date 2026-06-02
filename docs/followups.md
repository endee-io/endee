# Follow-ups

Loose ends discovered during work on other branches. Each item is something a future
ticket can pick up; nothing here is critical for current correctness.

## Crash harness coverage gaps

The harness has known coverage gaps. None of them were hit during the run that found the now-fixed bugs (see [single_txn_bug_investigation.md](./single_txn_bug_investigation.md)); they are listed so a reviewer can decide which to close before merge:

- **Concurrent writer threads outliving the kill window.** Writers are `daemon=True` with `join(timeout=5)`. If an HTTP call takes longer than 5s and the next cycle restarts on the same port, a stuck request could in principle land against the new server. The `RequestException` branch handles this in practice, but it isn't proven.
- **`fail_insert` silently drops potential bugs.** If the server ever returned non-200 *after* a partial commit, the harness removes the seq from inflight without recording it. The server's commit-or-abort design makes this theoretical, but the harness has no count-based check to catch it.
- **xenv `reconcile_inflight` absorbs half-states permissively.** It accepts any post-restart state for inflight ops as valid. A real half-state during a *non*-inflight op would be missed if the model drifts.
- **Concurrent kill timing is fixed.** Sleep `cycle_seconds` exactly then kill - no randomization of kill latency within a writer's request lifecycle.
- **Read-snapshot consistency is not exercised.** The harness only does point reads via `getVector`, not concurrent reader-during-writer-with-id-reuse.
- **`fsync_sanity.py` is informative, not validating.** It demonstrates the SIGKILL-vs-power-loss gap rather than catching a real data-loss bug. The dm-log-writes-style block-layer test in [§ Power-loss durability](#power-loss-durability---tested-passing) is the validating counterpart.
- **xenv cross-env sample size is modest.** ~240 ack'd creates over 12 cycles. The half-state window is small; more attempts would raise confidence.

## Power-loss durability - tested, passing

The earlier gap "power-loss path remains untested" is now closed. Result: when `single_txn` returns success on an insert, the bytes are genuinely on the device before the ack returns; a power cut a split second later does not lose the data.

- **Why this was hard.** Killing the program is not enough. SIGKILL terminates the process, but the kernel is still alive and quietly finishes writing dirty pages to disk afterwards - so SIGKILL makes the system look durable even when it isn't. A real power cut kills everything at once: in-flight kernel buffers vanish. `tests/crash_harness/fsync_drop.so` proves the gap empirically - it intercepts every `fsync`/`fdatasync`/`msync` and the SIGKILL harness still reports zero data loss because the kernel writeback thread completes the work.
- **Setup.** A small block layer that can be "frozen" at an exact instant (dm-log-writes style: records every write + fsync barrier, replays at any prefix length). Anything not genuinely on the device by the freeze point is gone, just like a real power cut. The `single_txn` server runs on top of it.
- **Sanity control (proves the harness can catch real loss).** Insert 100 records (server acks each). Freeze the device. Cut "power" and reboot. Result: all 100 records lost. This step matters: a passing test on a harness that cannot detect loss is meaningless.
- **Real test.** Insert 200 records (server acks each). Cut "power" without graceful shutdown. Reboot. Result: all 200 records survive - `getVector` returns each, `search` finds each, the server keeps accepting new inserts after recovery.
- **Conclusion.** `single_txn` does not lose acknowledged writes on power loss. Combined with the no-MDBX_MAPASYNC change documented in [docs/mdbx_shared_env_acid_revamp.md](./mdbx_shared_env_acid_revamp.md) § Durability Flags, every successful `mdbx_txn_commit` produces a steady commit that survives an immediate power cut.

## Production readiness summary

With the power-loss test passing, all four end-to-end gates hold on `single_txn`:

- Old data on the old binary: works as before.
- Old data on the new binary: refused at load and at `restoreBackup` with a clear pointer to `ndd-migrate-v0-to-v2`.
- New data on the new binary: add / update / delete / search / backup all work.
- New binary survives power loss without losing acknowledged data.

## Wire format: positional msgpack vs named-map

- **Current shape.** `HybridVectorObject`, `VectorObject`, `VectorResult`, `SparseVectorResult`, and the batch types in `src/utils/msgpack_ndd.hpp` all use `MSGPACK_DEFINE(...)`, which serialises the struct as a positional array. `HybridVectorObject` on the wire is `[id, meta, filter, norm, vector, sparse_ids, sparse_values]`.
- **Where it bit us.** Caught while writing the crash harness - Python decoders that treated the response as `dict["vector"]` silently got `None` and concluded the row was missing.
- **Trade-off, not a bug.** Positional arrays are smaller on the wire and faster to encode/decode. Existing clients already depend on this layout.
- **Future option.** `MSGPACK_DEFINE_MAP(...)` would make the wire format self-describing: clients decode by field name, new fields can be added without breaking old clients, field reordering becomes safe. Cost: one-time breaking change for every existing client, slightly larger payload.
- **When to consider switching.** Best paired with a major protocol bump (e.g. adding new fields to `HybridVectorObject` or `VectorResult` that older clients shouldn't need to handle) - that's the cheapest moment.
