# `op_log` vs. an Independent-File WAL

## Problem

`master` keeps the write-ahead log in a separate file (`<index>/wal.bin`, an `std::ofstream` with `flush()`) while data rows live in MDBX. The two are independent durability domains: each gets its own write, its own kernel buffering, and its own moment-of-arrival on disk. ACID requires atomic commit across all participants, and a user-space program on top of POSIX has no primitive to atomically couple two arbitrary inodes. So a crash in the gap between the two writes leaves on-disk state where the WAL and MDBX disagree, and recovery is forced to guess.

`single_txn` moves the log into an MDBX DBI named `op_log` ([src/storage/wal.hpp:51-77, 121-139](../src/storage/wal.hpp#L51-L139)). `wal.log(txn, …)` writes through the same `MDBX_txn` that writes the data rows; `mdbx_txn_commit` ([src/core/ndd.hpp:1284-1300](../src/core/ndd.hpp#L1284-L1300)) is the single atomic durability act for both. The disagreement window is unrepresentable by construction.

Two black-box tests in [`tests/wal_acid/`](../tests/wal_acid/) make this concrete at the HTTP API level: the same scripts run against both branches and produce opposite results. They are the executable form of the argument below.

## Seven things you might try (none are enough)

Before you read the deep dives below, the punch line in a table. Each row is an honest attempt to make a file-based WAL ACID-compliant; the right column is the failure window the attempt leaves open.

| # | Technique | What it buys you | Failure window that remains |
|---|---|---|---|
| 1 | Naive `flush()` to `ofstream` (master today) | Buffer → kernel page cache | Crash before kernel writes the page → either MDBX committed with no WAL trace, or WAL has an entry MDBX rolled back. |
| 2 | `fdatasync(wal_fd)` after every record | Forces the WAL bytes to the platter | Strengthens one half of a two-half pair. `fdatasync(wal_fd)` and `mdbx_txn_commit` are still two independent durability acts. Crash between them = same disagreement. |
| 3 | Log-ahead ordering (fsync WAL *before* MDBX commit) | The textbook WAL recipe; gives `MDBX row ⇒ WAL entry` | Phantom WAL entries when MDBX rolls back. Recovery cannot tell phantom-add from committed-then-deleted; **this is Bug B**. See deep dive below. |
| 4 | Log-behind ordering (fsync WAL *after* MDBX commit) | Inverts (3): gives `WAL entry ⇒ MDBX row` | Lost WAL entries when MDBX committed but trailing fsync didn't. **Silent HNSW staleness** — recall drops with no error signal. See deep dive below. |
| 5 | 2PC: PREPARE before MDBX commit, COMMIT after | Marker discipline; PREPARE-only means uncommitted, both means committed | "PREPARE only" is ambiguous: MDBX might have committed before the COMMIT marker's fsync, or never committed. The data side (MDBX) has no API to answer "did you commit lsn X?", so the ambiguity is unresolvable. See deep dive below. |
| 6 | Group-commit + checkpoint watermark | Per-record markers replaced by occasional checkpoint records | The checkpoint marker is *itself* a separate fsync, with the same crash window — just smaller blast radius. The recursion is built in. |
| 7 | Filesystem-level barriers (`O_DSYNC`, `sync_file_range`, ext4 `data=journal`, `renameat2`) | Strong intra-file guarantees from the kernel/fs | POSIX gives you intra-inode atomicity and intra-fs ordering. **No POSIX primitive atomically commits writes across two unrelated inodes.** Building one in user space means building a TM, i.e. reinventing MDBX. |

Deep dives on all seven follow. The first two are the on-disk basics, #3–#5 are the three serious recovery designs, #6 and #7 are the recursive defenses that try to engineer around the gap.

## Deep dive: naive `flush()` (#1) — what master ships today

```
1. ofs.write(...)         ← bytes copied to userspace stream buffer
2. ofs.flush()            ← write(2) — bytes copied to kernel page cache.
                          ← NO fsync.
3. begin MDBX txn, write id_map / vector_storage / ...
4. mdbx_txn_commit()      ← MDBX's own fsync inside.
```

The invariant established by `ofs.flush()` is essentially nothing: it guarantees the bytes have left the userspace stream buffer and reached the kernel. It does **not** guarantee they reached disk, nor that they will survive a power loss.

**Two failure modes**:

1. **Power loss before the kernel writes the dirty page to disk.** The WAL bytes are in RAM only. After a power-cycle, `wal.bin` is empty or partial. If MDBX did commit before the power loss (MDBX has its own fsync inside `mdbx_txn_commit`), you now have a row in MDBX with no log entry. HNSW recovery has nothing to replay; the row is invisible to search forever — silent staleness.
2. **SIGKILL of the process.** Bytes survive in the kernel page cache (the kernel is still alive). But the WAL write and MDBX's commit write are two independent dirty pages. The kernel can flush them in any order to the storage device. A subsequent host crash or reboot might persist one and lose the other.

**This is what master ships today**, with one additional twist: master also opens MDBX with `MDBX_MAPASYNC`, which makes MDBX commits *also* unsteady. So the asymmetry isn't "strong data + weak log" — it's "two weak halves" that can disagree in either direction. See [`docs/mdbx_shared_env_acid_revamp.md`](mdbx_shared_env_acid_revamp.md) for the `MAPASYNC` removal in `single_txn`.

**Takeaway**: `flush()` is a contract about *where the bytes have moved*, not whether they will survive a crash. The naive design has neither atomicity *nor* per-piece durability. This is the failure mode that [`tests/wal_acid/01_no_separate_wal_file.py`](../tests/wal_acid/01_no_separate_wal_file.py) catches structurally: it asserts there is no such file at all on disk after operations.

## Deep dive: `fdatasync(wal_fd)` after every record (#2)

```
1. write(wal_fd, ...)
2. fdatasync(wal_fd)      ← WAL bytes are on the platter (per-fd durability).
3. begin MDBX txn
4. mdbx_txn_commit()      ← MDBX's own fsync inside (per-fd durability).
```

The invariant: *each file's bytes are independently durable*. WAL bytes won't be lost in a power cut. MDBX bytes won't be lost either. Strong improvement over (#1).

But "individually durable" is not "jointly atomic". `fdatasync(wal_fd)` and `mdbx_txn_commit` target two different inodes. There is no ordering guarantee between them across the two-write boundary, and no atomicity:

- After step 2 returns, `wal.bin` is durable. MDBX commit hasn't happened yet.
- Crash between step 2 and step 4 → `wal.bin` claims `ADD(N)`, MDBX has no row at N.
- This is exactly attempt 3's failure (log-ahead phantom entry), arrived at by a slightly stronger route.

**Why stronger fsync doesn't help**: the problem is *coupling between two writes*, not *durability of each piece*. Each piece can be arbitrarily durable; the moment between them remains observable. POSIX has no `fsync_pair(fd1, fd2)` syscall and no documented way to make two `fsync` calls atomic.

A more subtle point: even the *ordering* of the two fsyncs isn't strong enough. SSDs internally reorder writes via the FTL. Filesystem journals can defer their own metadata flushes. The barrier semantics POSIX gives you are per-fd, not cross-fd, and the storage stack underneath may further relax them.

**Takeaway**: `fdatasync` is the right primitive when you need a single piece of state to be durable. It is the wrong primitive when you need two pieces of state to be durable *together*. For "together", you need them in one I/O atom — which on a single machine means one transaction in one storage engine.

## Deep dive: log-ahead ordering (#3) — the textbook recipe

```
1. append ADD(N) to wal.bin
2. fdatasync(wal_fd)                          ← log durable
3. begin MDBX txn, write id_map / vector_storage / ...
4. mdbx_txn_commit()                          ← data durable
```

The invariant established by the ordering is **`MDBX row durable ⇒ WAL entry durable`** — one direction.

**Hidden assumption that breaks in our system.** The classical WAL recipe (Postgres, InnoDB, ARIES) works because *the log is the authoritative database*; the data files are a checkpointed cache, regenerable by replaying the log. A WAL entry that lacks a corresponding data write is *fine* — replay will write it. A data write without a WAL entry is *impossible* by construction.

Our system violates this assumption twice over:

1. **MDBX is itself a fully transactional store** with its own internal WAL/COW semantics. MDBX commits are complete durability events; the mmap-backed B-tree pages are *not* regenerable from our external WAL.
2. **The external WAL drives HNSW**, an in-memory graph saved to a separate `.bin` file. On recovery, HNSW is reconstructed by replaying every *committed* op into the graph.

So recovery has to answer, per WAL entry: *"did MDBX commit the corresponding row?"* If yes → replay into HNSW. If no → skip. This requires the *opposite* implication — `WAL entry ⇒ MDBX row` — which log-ahead ordering does *not* provide. The code has to *infer* the answer by reading MDBX state.

**Why that inference is unsound.** MDBX state has moved on since the original commit. Concretely, given a numeric_id N:

| Time | Event | MDBX state at N | `deleted_ids` pool | WAL contents |
|------|-------|----------------|--------------------|--------------|
| t₀   | (prior cycle deleted N) | empty | `[…, N, …]` | ∅ |
| t₁   | `addVectors("v_X")` consumes N from pool, **commits** | row B | `[…]` | `ADD(N)` |
| t₂   | `deleteByNumericIds([N])`, **commits** | empty | `[…, N]` | `ADD(N), DEL(N)` |
| t₃   | SIGKILL | empty | `[…, N]` | `ADD(N), DEL(N)` |
| t₄   | recovery starts | | | |

Recovery hits `ADD(N)`, looks at MDBX: no row at N. Naive inference: *"the add must have failed, reclaim N to the deleted pool."* `reclaim_failed_ids` is called, which calls `add_to_deleted_ids` — appending N to a pool **that already contains N** because the legit DELETE at t₂ put it there. The pool now has N twice. The next `create_ids_batch` pops both copies and hands them to two different string_ids in the same batch. Two strings share one row in `vector_storage`. Last write wins. The first string returns the second string's bytes. **Silent cross-document corruption.** This is the trace in [docs/single_txn_bug_investigation.md § Bug B](single_txn_bug_investigation.md#bug-b---duplicate-numeric_id-in-deleted_ids_key-after-crash-recovery).

The reason no inference rule can save you: MDBX state at N is *empty for the same reason* whether the ADD never committed or the ADD committed and the DEL removed it. MDBX has no memory of the difference. The WAL contains both events but the recovery code as written cannot disambiguate them across many add/delete cycles.

There is also no safe "blind replay" rule. Blindly replaying `ADD(N)` into HNSW when MDBX rolled it back creates a label-N node in HNSW with no row in MDBX. Later a fresh `addVectors` reuses N for a *different* string. HNSW now has two label-N nodes; search returns either, randomly. Cross-document result confusion is worse than no recovery.

**Takeaway.** Log-ahead establishes a one-way arrow. The architecture needs a biconditional. Ordering cannot supply the missing direction, because the information needed has been erased from MDBX by subsequent legitimate operations.

[`tests/wal_acid/02_phantom_wal_causes_no_inconsistency.py`](../tests/wal_acid/02_phantom_wal_causes_no_inconsistency.py) constructs exactly this on-disk state and asserts the downstream API-level inconsistency it causes on master.

## Deep dive: log-behind ordering (#4)

```
1. begin MDBX txn, write id_map / vector_storage / ...
2. mdbx_txn_commit()                          ← data durable
3. append ADD(N) to wal.bin
4. fdatasync(wal_fd)                          ← log durable
```

The invariant flips: **`WAL entry durable ⇒ MDBX row durable`**.

Now the reconciliation is trivial. Every WAL entry recovery sees corresponds to a real MDBX commit. Replay each into HNSW with no MDBX consultation. **Bug B is impossible.**

The catch: you gained one direction by giving up the other. The failure becomes:

```
mdbx_txn_commit returns successfully    ← data row N durable in MDBX
SIGKILL                                  ← process killed before step 4
... wal.bin never received the entry
restart
recovery reads WAL: no entry for N
HNSW never inserts node N
```

On-disk state: MDBX has row N, `getVector("v_X")` returns the bytes. HNSW has no node N. `search(v_X_embedding)` will not return it. **The vector exists and is retrievable by id, but is invisible to vector search.** Recall drops by one vector per lost commit-to-fsync gap.

**Why this is, in practice, worse than Bug B.** Log-ahead failures are loud: wrong bytes get returned, customers email within hours, monitoring catches it. Log-behind failures are silent:

- Every byte returned is correct. No corruption.
- No errors, no exceptions, no warnings.
- `getVector("v_X")` works. `search` just… doesn't find it.
- Recall drifts down over time across crash events (kernel patches, OOM kills, node loss).
- Standard quality dashboards drift downward with no attributable cause.

You cannot shrink the gap to zero. The crash window between `mdbx_txn_commit` return and `fdatasync(wal_fd)` return is two independent kernel operations; the CPU can be preempted, page-faulted, power-cut between them. And the gap is *invisible*: the WAL has no record of the missed entry (that's the point), and MDBX has no record of "I committed something the WAL is missing" (MDBX doesn't know about the WAL). Neither side can flag the inconsistency.

## Deep dive: 2PC PREPARE/COMMIT (#5) — the strongest defense

```
1. append PREPARE(lsn, ADD(N)) to wal.bin
2. fdatasync(wal_fd)
3. mdbx_txn_commit
4. append COMMIT(lsn) to wal.bin
5. fdatasync(wal_fd)
```

Recovery rule: replay only records whose PREPARE has a matching COMMIT. Crisp, standard, used by every distributed-DB textbook.

Enumerate the four post-crash states:

| Marker state | Real-world MDBX state | Verdict |
|---|---|---|
| neither | nothing happened | ignore — correct |
| both | MDBX committed | replay — correct |
| **PREPARE only** | **MDBX rolled back** *or* **MDBX committed, COMMIT-fsync lost** | **undecidable** |
| COMMIT only | impossible under ordering (treat as corruption) | n/a |

The "PREPARE only" row is fatal. Two real-world states project onto the same on-disk state. Any recovery rule you pick:

- *Ignore PREPARE-only* → wrong when MDBX did commit (HNSW stale; back to attempt 4's silent corruption).
- *Replay PREPARE-only* → wrong when MDBX rolled back (phantom; back to attempt 3's Bug B).
- *Inspect MDBX to decide* → exactly the inference from attempt 3, which is unsound for the same reason (committed-then-deleted is indistinguishable from never-committed).

**Why 2PC works in distributed DBs and doesn't work here.** In classical 2PC every participant durably records its *vote* and the *coordinator's decision*. After a crash, the coordinator can call participants and ask, "did you commit lsn X?" The participants have a transaction-state record to answer from.

MDBX, used as a data store, has no such record. It does not remember "I committed transaction lsn X" in a way you can query from outside. You would have to *add* such a record — but that record would itself be metadata living in MDBX, written in the same transaction that mutates the data, identified by an LSN that links back to the WAL. **That is exactly what `op_log` is.** The two-TM design collapses back to one TM, with extra fsync overhead and a redundant external file.

The strict statement: 2PC works between two equal transaction managers. You cannot run 2PC between a transaction manager and a passive byte store.

The disagreement window is identical to attempt 1's: every defensive technique you'd layer in still leaves a moment between two independent durability acts. The defenses bought nothing.

## Deep dive: group commit + checkpoint watermark (#6)

```
... many WAL appends, batched, no per-record fsync ...
... many MDBX commits ...
... periodically:
        append CHECKPOINT(lsn = N) to wal.bin
        fdatasync(wal_fd)
```

The idea: stop paying per-record 2PC overhead. Instead write an occasional `CHECKPOINT(N)` marker meaning *"every WAL entry with lsn ≤ N is known durable in MDBX"*. Recovery only replays entries above the last checkpoint. Lower fsync rate, smaller blast radius per crash.

Two ways to place the checkpoint, **both broken**:

**Option (a) — write the checkpoint *before* the corresponding MDBX commits.** Then a crash between the checkpoint fsync and the next MDBX commit leaves a checkpoint on disk that claims durability MDBX never confirmed. Recovery trusts the checkpoint, skips the entries it should have replayed, and you're back in the "WAL says yes, MDBX rolled back" state of attempt 3.

**Option (b) — write the checkpoint *after* MDBX commits.** Same gap as attempt 4 (log-behind), just at checkpoint granularity. A crash between MDBX's commit and the checkpoint fsync loses the checkpoint. Recovery sees a non-truncated WAL, replays old entries — including entries MDBX has since *legitimately deleted*, which takes you straight back to Bug B.

Either way, the checkpoint marker is itself a piece of metadata that has the same atomicity problem you were trying to solve. **You moved the problem; you didn't eliminate it.** The window shrank but doesn't close.

Operational cost on top of that: between checkpoints, the WAL is unbounded, and recovery must replay every entry since the last checkpoint. The "deletion" of stale WAL entries now waits for a subsequent checkpoint. You've added lag to every operational concern (WAL size, recovery time, log rotation) in exchange for a window that's smaller but still real.

**Takeaway**: any "atomicity escape hatch" you write into the file WAL inherits the cross-file atomicity problem. **You cannot bootstrap atomic cross-file durability from non-atomic primitives** — the recursion is built in. Each new mechanism is itself a non-atomic write.

## Deep dive: filesystem-level barriers (#7)

The kernel and filesystem expose several primitives that *look* like they might close the gap:

- **`O_DSYNC` on the WAL fd** — every `write()` returns only when the data is durable.
- **`sync_file_range(fd, off, len, ...)`** — flush a specific byte range with controlled `WAIT_BEFORE | WRITE | WAIT_AFTER` semantics.
- **Mount the data dir with ext4 `data=journal`** — data writes pass through the filesystem journal, ordered with metadata.
- **`renameat2(..., RENAME_EXCHANGE)`** (Linux ≥ 3.15) — atomic swap of two filenames.

Each strengthens *intra-inode* or *intra-fs* guarantees:

- `O_DSYNC` is stronger per-write durability on one fd.
- `sync_file_range` is ordering within one file.
- `data=journal` is ordering and durability within the fs journal, but the application's writes to two unrelated inodes are still independent operations to the journal.
- `renameat2 RENAME_EXCHANGE` is an atomic name-pair swap; the unit of atomicity is the rename, not the content of two writes.

**None of these provides a cross-inode atomic commit.** POSIX explicitly does not guarantee that two `fsync`s on two different fds happen in any particular relative order, nor that the two writes are atomically visible together after a crash. Linux fs journals durify the journal commit, not user-space writes to two unrelated inodes.

What about more exotic moves?

- **Write both records to a single combined journal file** — but then you're building your own transaction manager on top of the FS, with the same write-ordering puzzles to resolve internally. You've reinvented MDBX, badly.
- **Content-address the combined state** (write one log entry whose hash is the "did this commit" predicate) — that's still a single-store TM with extra steps.
- **Have each fd record a back-pointer to the other** — but the back-pointer is itself a write that has the same atomicity problem.

Every direction terminates at the same wall: cross-file atomic commit is not a POSIX service. Building it in user space means building a transaction manager. **MDBX already is a transaction manager. Put the log inside it.**

**Takeaway**: filesystem primitives give you intra-file atomicity and intra-fs ordering. They do not give you cross-file atomic commit. There is no syscall, no mount option, no `renameat2` trick that closes the gap. The only path is to make one of the two files unnecessary — which is exactly what `op_log` does.

## The theorem

> **Atomic commit is the property of having a single durable "now-it-is-done" moment, and that moment is owned by exactly one component — the transaction manager. You cannot have two.**

"Atomic" means the system is either before-commit or after-commit, with no observable in-between. That requires a single durable write to define the transition. If two stores each own their own commit moment, the world can be observed between them — by a power loss, a SIGKILL, any crash — and that in-between state is the disagreement we keep hitting.

To get atomicity across N participants on a single machine, the options collapse to:

1. **One TM owns durability.** Everyone else writes "operations" the TM journals as part of its commit. MDBX is the TM for the data rows. Moving the op_log into MDBX makes it the TM for the log too. One commit moment for both. ✓
2. **Two-phase commit between two real TMs**, each with its own durable transaction-state record. Requires MDBX to expose "did you commit lsn X?" It doesn't. Adding that = op_log in MDBX. Collapses to (1).
3. **Distributed consensus.** Useless single-node.

There is no fourth option. Two independent durability domains on the same machine cannot be atomically coupled by any amount of user-space discipline. **The WAL belongs inside the TM, full stop.**

## What `single_txn` actually does

The op_log is an MDBX DBI named `"op_log"` opened with `MDBX_CREATE | MDBX_INTEGERKEY` ([src/storage/wal.hpp:51-77](../src/storage/wal.hpp#L51-L77)). `WriteAheadLog::log(txn, entries)` ([src/storage/wal.hpp:121-139](../src/storage/wal.hpp#L121-L139)) calls `mdbx_put` on the same `MDBX_txn*` the caller is using for data writes, with `MDBX_APPEND` against a monotonic sequence key. The caller's eventual `mdbx_txn_commit` is the *single* durability act for the data rows and the log entry together. If the commit succeeds, both are durable; if the commit aborts or the process dies before commit, both are absent. No window. No 2PC. No fdatasync of a second file.

If the commit aborts mid-flight, `wal.readEntries()` will be empty *and* MDBX will have no row for `v_X` — both states are products of the same atomic act. There is no on-disk file in which a phantom log entry could survive: see [`tests/wal_acid/01_no_separate_wal_file.py`](../tests/wal_acid/01_no_separate_wal_file.py) for the empirical check.

## "What about the dedup at `add_to_deleted_ids`?"

The dedup at [src/storage/id_mapper.hpp:384-389](../src/storage/id_mapper.hpp#L384-L389) checks whether each id is already present in `DELETED_IDS_KEY` before appending. It is defense-in-depth against one specific downstream symptom of the file-WAL disagreement (the duplicate-numeric_id corruption traced in Bug B). It is **not** a fix for the disagreement itself. Other downstream symptoms — the silent HNSW staleness under log-behind, the cross-document HNSW label collision under blind replay — are not papered over. The dedup is a workaround for a structural problem; `op_log` eliminates the structural problem.

## Executable proof — [tests/wal_acid/](../tests/wal_acid/)

A self-contained folder of Python tests that hit the `ndd` HTTP API. **The same scripts run unmodified on both `master` and `single_txn` with opposite results.** No source dependencies, no CMake changes, stdlib only.

```bash
# Point at the single_txn binary  →  Summary: 2 passed, 0 failed
NDD_BINARY=/abs/path/to/build/ndd-avx2 \
    ./tests/wal_acid/run_all.sh

# Point at the master binary       →  Summary: 0 passed, 2 failed
NDD_BINARY=/abs/path/to/build-master/ndd-avx2 \
    ./tests/wal_acid/run_all.sh
```

Two tests:
- `01_no_separate_wal_file.py` — asserts no `wal.bin` exists in the data dir after basic ops. Fails on master (separate file exists), passes on single_txn (log is inside MDBX).
- `02_phantom_wal_causes_no_inconsistency.py` — injects a phantom on-disk WAL entry between server restarts and asserts `search` and `getVector` still agree about the vector. Fails on master (phantom is replayed, HNSW markDeletes the row, search misses it, getVector still returns it), passes on single_txn (no separate WAL file to inject into).

## References

- [docs/single_txn_bug_investigation.md](single_txn_bug_investigation.md) — Bug B production trace and the dedup landing.
- [docs/mdbx_shared_env_acid_revamp.md](mdbx_shared_env_acid_revamp.md) — the broader shared-env ACID rework this change is part of.
- [src/storage/wal.hpp](../src/storage/wal.hpp) — `WriteAheadLog` (the op_log DBI implementation).
- [src/core/ndd.hpp:281-361](../src/core/ndd.hpp#L281-L361) — `recoverFromWAL`, the replay path.
