# Crash harness

End-to-end test of the `single_txn` branch's ACID and crash-consistency claims.
Spawns the server in a child process, drives a deterministic HTTP workload,
`SIGKILL`s the server at a chosen point, restarts on the same data directory,
and verifies every acknowledged operation against an in-process shadow model.

Verification reads two independent surfaces of the database:
- **MDBX side:** `getVector` (point read via `id_map` → `vector_storage`).
  Exhaustive over every acked seq.
- **HNSW side:** `search` (graph traversal). Sample-based; for each sampled
  acked-present seq, query with the original vector and assert the id appears
  in the top-k. For each acked-deleted seq, assert it does NOT appear.

The two-surface check is critical for correctness. An earlier version of the harness
checked only `getVector` and reported "ALL CHECKS PASSED" on workloads that
were silently losing the majority of HNSW labels across restarts. The bugs
listed in [Findings](#findings) are visible only when both surfaces are checked.

## Files

| Path | Purpose |
|---|---|
| `run_harness.py` | Insert/delete workload against one index; sequential and concurrent modes |
| `xenv_harness.py` | Cross-env workload: create/delete/insert across many indices |
| `fsync_drop.c` / `fsync_drop.so` | LD\_PRELOAD shim that no-ops `fsync`/`fdatasync`/`msync`/`syncfs` |
| `fsync_sanity.py` | Demonstrates the harness's coverage limit re: power loss |

## Build

```
cmake -S . -B build-test -DENABLE_TESTING=ON -DUSE_AVX2=ON
cmake --build build-test -j4
gcc -shared -fPIC -O2 -o tests/crash_harness/fsync_drop.so \
    tests/crash_harness/fsync_drop.c -ldl   # only needed for fsync_sanity
```

Python dependencies: `msgpack`, `requests` (both already installed).

## What each mode tests

### `run_harness.py --mode sequential` - Gap 2 (HNSW save boundary) + baseline

One HTTP call at a time. Each cycle runs N ops, then SIGKILL, then restart.
With `--cycle-ops-min`/`--cycle-ops-max` set, some cycles cross
`SAVE_EVERY_N_UPDATES = 10000`, which triggers an HNSW save mid-cycle and
exercises the "saved HNSW + residual op\_log" recovery branch (different from
"pure op\_log replay").

Reproducer:
```
python3 tests/crash_harness/run_harness.py \
  --binary $PWD/build-test/ndd-avx2 \
  --cycles 4 --cycle-ops-min 8000 --cycle-ops-max 12000 --seed 11
```

### `run_harness.py --mode concurrent` - Gap 1 (kill mid-write)

N writer threads share state under a lock and hammer the server. The main
thread sleeps `--cycle-seconds` then sets a stop flag and kills the server.
This puts the SIGKILL inside the writers' request lifecycle - the exact
window where "HTTP returned 200 but disk fsync not done" *would* matter, if it
were possible (it isn't, but the test is still valuable: it catches half-applied
multi-row commits and concurrency races).

Reproducer:
```
python3 tests/crash_harness/run_harness.py \
  --binary $PWD/build-test/ndd-avx2 \
  --mode concurrent --writers 4 --cycle-seconds 2 \
  --cycles 5 --seed 23
```

### `xenv_harness.py` - Gap 4 (cross-env atomicity)

The `single_txn` branch atomically commits within one per-index MDBX env, but
`createIndex` / `deleteIndex` / `restoreBackup` also touch the global catalog
in a separate env. The doc
[docs/mdbx\_shared\_env\_acid\_revamp.md][acid] explicitly notes this gap.

Workload: create/insert/delete indices with deterministic names. After each
restart, list the catalog (`/api/v1/index/list`), `ls` the data directory, and
assert: every acked-create has both, every acked-delete has neither, no name
exists in only one. Sequential and concurrent modes both supported.

Reproducer:
```
python3 tests/crash_harness/xenv_harness.py \
  --binary $PWD/build-test/ndd-avx2 \
  --mode concurrent --writers 4 --cycle-seconds 1.5 \
  --cycles 12 --seed 41
```

### `fsync_sanity.py` - Gap 3 (fsync interception, expected-null)

LD\_PRELOADs `fsync_drop.so` into the server, inserts N vectors (all acked),
SIGKILLs, restarts *without* the preload, and counts how many acked vectors
survived. The shim verifies it's loaded with a stderr banner.

Result on this machine: zero data loss. This is not a bug - SIGKILL only
terminates the process; the kernel's page cache survives. Dirty pages from
MDBX's mmap region are written back by the kernel's writeback thread regardless
of whether `fsync` was called. fsync makes the *caller* wait; it doesn't
control what eventually reaches disk after a clean process death.

Real power-loss simulation needs one of (all require root):
- `dm-log-writes` block device for prefix-replay
- `echo 3 > /proc/sys/vm/drop_caches` between SIGKILL and restart
- VM hard-reset between phases

The script prints this explanation when it gets the expected-null result.

```
python3 tests/crash_harness/fsync_sanity.py \
  --binary $PWD/build-test/ndd-avx2 --inserts 2000
```

**Power loss is now actually tested** out-of-tree using the `dm-log-writes`
option above. See [docs/followups.md § Power-loss durability](../../docs/followups.md#power-loss-durability---tested-passing)
for the methodology and the result (200 acked inserts survive a simulated
power cut). `fsync_sanity.py` here is left in place to document *why*
SIGKILL alone is not enough.

## Findings (resolved)

Two real bugs and one harness artefact were detected during single_txn work and have been fixed. Full investigation, root causes, and fixes are in [docs/single_txn_bug_investigation.md][bug-investigation]; quick index below.

### A. HNSW edges rewired on reused numeric_id (fixed)

- **Symptom**: concurrent stress, 4 writers × 2s × 5 cycles, seed=23 - search-miss rate grew monotonically (2.5% → 24% → 42% → 52.5% → 66.5%). MDBX `getVector` was intact; only `search` was broken because the level-0 graph fractured into disconnected components.
- **Root cause**: `id_mapper`'s `create_ids_batch` treated a reused-after-delete numeric id as a string-id *update*, sending fresh inserts down `addPoint<false>` (rewire) instead of `addPoint<true>`.
- **Fix**: drop the `is_reused` override in `create_ids_batch`; clear `labelLookup_[label]` in HNSW `markDelete`; skip marked-deleted slots when rebuilding `labelLookup_` in `loadIndex`.

### B. Duplicate numeric_id in DELETED_IDS_KEY after crash recovery (fixed)

- **Symptom**: sequential, 5 cycles × 100 ops, seed=1 - after cycle 3 restart, `getVector("v_00000288")` returned `dv(291)` byte-exact. Two string ids ended up sharing one numeric id in the same batch; last write wins on MDBX.
- **Root cause**: `recoverFromWAL` re-pushed a numeric id into `DELETED_IDS_KEY` even when it was already there (the "add then delete then kill" case), so the next `create_ids_batch` handed it out twice.
- **Fix**: `add_to_deleted_ids` now deduplicates against the existing list before appending.

### Not a bug - workload artefact (harness fix)

- **Symptom**: after Bug A's fix, concurrent stress still showed monotonic miss-rate growth.
- **Cause**: the harness's `deterministic_vector(seq)` generator placed all inserts on a near-1D 8-dimensional manifold (consecutive seqs shared 7 of 8 coordinates), triggering HNSW heuristic-2 pruning into disconnected components.
- **Fix**: `tests/crash_harness/run_harness.py` rewritten to use independent per-coordinate hashing.

After the fixes: the full sweep (6 sequential seeds × 5 cycles × 100 ops, concurrent 4-writer with and without deletes × 5 cycles × 2s, xenv concurrent × 12 cycles) plus `ctest --output-on-failure` all pass.

## What the harness still does NOT check

These were called out during my own audit and are recorded in
[docs/followups.md][followups]. None of them blocked finding A or B above, but
they should be closed before claiming full ACID coverage.

1. **Power-loss durability** is structurally unreachable from userspace
   without root. SIGKILL ≠ power loss; see `fsync_sanity.py`.
2. **Concurrent writer threads can outlive the kill window** in theory
   (`daemon=True`, `join(timeout=5)`); the `RequestException` branch handles
   it in practice but it's not proven.
3. **`fail_insert` silently drops potential bugs** if the server ever
   returned non-200 after a partial commit. Mitigated by commit-or-abort, not
   robust against a future regression.
4. **`xenv` `reconcile_inflight` is permissive** about half-states for
   inflight ops; only catches half-states on *committed* ops.
5. **Concurrent kill timing is fixed** (sleep then kill) - no randomization
   within a request lifecycle.
6. **Read-snapshot gap is untested.** Doc explicitly lists this; harness only
   does point reads, not concurrent reader-during-id-reuse-writer.
7. **`fsync_sanity` is informative, not validating.** A control-injection
   test (deliberately corrupting the data dir post-kill, confirming verifier
   flags it) would prove the verification machinery itself works.
8. **Wire format consideration** unrelated to crash testing: `MSGPACK_DEFINE`
   on `HybridVectorObject` uses positional arrays, which silently broke a
   first version of the Python client. `MSGPACK_DEFINE_MAP` is more
   forward-compatible at the cost of a one-time breaking change.

## How to extend

- **A new failure surface** (e.g. filter rows, sparse postings): add a probe
  method to `Client` and a check loop in `verify_durability`. Both surfaces are
  exhaustive for MDBX state; HNSW-style sampling is fine for anything
  approximate.
- **A new workload** (e.g. `restoreBackup`): write a new `*_harness.py` that
  reuses `ServerProc` and `free_port` from `run_harness.py`. The pattern is
  always the same: reserve-then-commit shadow state, run, SIGKILL, restart,
  reconcile inflight, verify, repeat.
- **A new failure mode** (e.g. random network drops): use the
  `--server-ld-preload` flag on `run_harness.py` with a shim that intercepts
  whatever syscall you want to perturb. `fsync_drop.c` is a working template.

[acid]: ../../docs/mdbx_shared_env_acid_revamp.md
[followups]: ../../docs/followups.md
[bug-investigation]: ../../docs/single_txn_bug_investigation.md
