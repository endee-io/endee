# `tests/wal_acid/` — branch-independent WAL ACID demonstrations

A folder of standalone Python tests that hit the running `ndd` server over
HTTP. **The folder is fully self-contained** — no dependency on any
specific branch's source tree, no project headers, no `pip install`,
stdlib only. The same scripts run unmodified against `master` and
`single_txn`; the branch's behaviour is what differs.

| Test | Expected: `single_txn` | Expected: `master` |
|------|---|---|
| `01_no_separate_wal_file.py` | **PASS** — log lives inside `mdbx.dat` as the `op_log` DBI. | **FAIL** — `wal.bin` exists in every index dir. |
| `02_phantom_wal_causes_no_inconsistency.py` | **PASS** — no separate WAL file to inject into; the op_log entry would have committed atomically with the data. | **FAIL** — phantom DELETE in `wal.bin` is replayed by recovery; HNSW markDeletes the row, `search` misses it, `getVector` still returns it. |

## The intended workflow

You have two pre-built binaries: `build/ndd-avx2` (single_txn) and
`build-master/ndd-avx2` (master). You want to run the same tests against
each and see opposite results.

```bash
# From any branch / any working tree that has this folder:

# Single_txn binary:
NDD_BINARY=/abs/path/to/build/ndd-avx2 \
    ./tests/wal_acid/run_all.sh
# → Summary: 2 passed, 0 failed

# Master binary:
NDD_BINARY=/abs/path/to/build-master/ndd-avx2 \
    ./tests/wal_acid/run_all.sh
# → Summary: 0 passed, 2 failed
```

## If you're on master and these test files don't exist there

Copy the folder over. That's it. No CMake changes, no recompile.

```bash
# From the master worktree:
cp -r /path/to/single_txn/tests/wal_acid tests/wal_acid

# Then run as above.
```

The harness has no source-tree dependencies — it spawns the binary you
point it at, hits its HTTP API, and reads the data directory it created.

## Running individual tests

```bash
NDD_BINARY=/abs/path/to/ndd-avx2 \
    python3 tests/wal_acid/01_no_separate_wal_file.py

NDD_BINARY=/abs/path/to/ndd-avx2 \
    python3 tests/wal_acid/02_phantom_wal_causes_no_inconsistency.py
```

Each test allocates its own free port and temp data dir, and cleans up
after itself. They can run in parallel.

## Why these tests, in this form

The fundamental disagreement they demonstrate is between two durability
domains (the WAL file and the MDBX env) — on `master` independent, on
`single_txn` collapsed into one MDBX transaction. The cleanest
deterministic way to expose the disagreement at the API layer is:

1. Bring the system to a known-good state through the API.
2. Stop the server gracefully (so on-disk state is fully flushed).
3. **Inject the on-disk state that a crash inside the WAL-vs-MDBX gap
   would have left**: append a fake record to any standalone WAL file
   in the data dir.
4. Restart the server. The first API call to the index triggers
   `recoverFromWAL` which consumes any on-disk WAL entries.
5. Assert via the HTTP API that the system is internally consistent.

On `master`, step 3 finds a `wal.bin` and writes to it. On `single_txn`,
step 3 finds nothing to write to — there is no on-disk file representing
an "uncommitted log entry" because no such thing can exist.

## See also

- [docs/op_log_vs_wal.md](../../docs/op_log_vs_wal.md) — theory: 7-row
  attempts table, deep dives on log-ahead / log-behind / 2PC, the
  single-TM theorem.
- [docs/single_txn_bug_investigation.md](../../docs/single_txn_bug_investigation.md) — the
  original production trace of Bug B that motivated this work.
