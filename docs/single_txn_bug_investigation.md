# single_txn branch: bug investigation writeup

Record of two real bugs fixed during single_txn crash-harness work, plus one workload artefact and one defensive fix. Bugs are closed; this file is the durable record of what was wrong, how it was fixed, and where the fixes live.

## Bug A - reused numeric_id treated as string_id update

- **Symptom**: concurrent stress, HNSW search misses grow with total ops while MDBX `getVector` stays intact. Level-0 graph fractures into disconnected components - labels in `labelLookup_` outnumber labels reachable from `entryPoint_`, gap grows across cycles.
- **Root cause**: `IDMapper::create_ids_batch` had an `is_reused` override that forced `is_new_to_hnsw = false` whenever the numeric id came from the deleted pool. That sent a fresh string_id paired with a recycled numeric_id down `addPoint<false>` (rewire), wiping the previously-marked-deleted slot's edges and rebuilding them via search-from-entry. Over many inserts the graph fractured.
- **Fix**:
  - `src/storage/id_mapper.hpp` `create_ids_batch`: dropped the `is_reused` override. Recycled numeric ids paired with new string ids now go through `addPoint<true>` and emit `VECTOR_ADD` in the WAL, which is what was always intended.
  - `src/hnsw/hnswalg.h` `markDelete`: clear `labelLookup_[label] = INVALID_ID`. The slot stays in `dataBaseLayer_` with `DELETE_MARK` set but no longer "owns" the external label.
  - `src/hnsw/hnswalg.h` `loadIndex`: skip marked-deleted slots when rebuilding `labelLookup_` so the invariant survives save+load.

## Bug B - duplicate numeric_id in DELETED_IDS_KEY after crash recovery

- **Symptom**: seed=1, sequential, cycle 3 - `getVector("v_00000288")` returns the vector that was inserted at `v_00000291`, byte-exact. Two string ids share one numeric id in the same batch; last MDBX write wins.
- **Root cause**: when a numeric id was allocated, used, and legitimately deleted all within one HNSW save window, then the process was killed mid-window, recovery double-pushed the id into the deleted pool:
  1. Cycle 2: id 234 was previously deleted, sits in `DELETED_IDS_KEY`.
  2. Cycle 2: insert consumes 234, commits `id_map[v_X]=234`, `vector_storage[234]=bytes`, `VECTOR_ADD(234)`.
  3. Cycle 2: delete commits id_map removal, vector_storage removal, pushes 234 back into `DELETED_IDS_KEY` (once), `VECTOR_DELETE(234)`.
  4. SIGKILL before save. WAL is intact, 234 sits in `DELETED_IDS_KEY` exactly once.
  5. Cycle 3 restart: `recoverFromWAL` sees `VECTOR_ADD(234)`, checks `vector_storage.get_vector(234)`, finds empty, classifies as failed, pushes 234 to `failed_vector_add_ids`.
  6. `reclaim_failed_ids` -> `add_to_deleted_ids([234])` appends 234 without deduping. `DELETED_IDS_KEY` now contains 234 twice.
  7. Next batch's `getDeletedIds(10)` hands 234 to two different string ids in the same `create_ids_batch`.
- **Fix**: `src/storage/id_mapper.hpp` `add_to_deleted_ids` deduplicates against the existing list before appending:

  ```cpp
  std::unordered_set<idInt> seen(existing.begin(), existing.end());
  for(idInt id : ids) {
      if(seen.insert(id).second) {
          existing.push_back(id);
      }
  }
  ```

  `#include <unordered_set>` added. The "aborted partial add" pattern `reclaim_failed_ids` was originally written for still works; the "add then delete then kill" pattern is now idempotent.

## Not a bug - HNSW workload artefact

- **Symptom**: after Bug A's fix, concurrent stress still showed monotonic miss-rate growth across cycles. `bfs_reachable` gap grew while in-degree counters stayed clean.
- **Cause**: the harness's `deterministic_vector(seq)` returned `[f(seq+0), f(seq+1), ..., f(seq+7)]`, so `dv(s)` and `dv(s+1)` shared 7 of 8 coordinates. All inserts landed on a near-1D manifold in 8-D space - exactly the pathological input for HNSW heuristic-2 pruning, which favours diversity over connectivity and fractures the level-0 graph.
- **Fix**: `tests/crash_harness/run_harness.py` `deterministic_vector` rewritten to use independent per-coordinate hashing. Test-harness fix, not a database fix.
- **Why kept on file**: the same diagnostic signal ("BFS gap grows monotonically, in-degree fine") will fool anyone who doesn't know the harness used to produce it. Real workloads with embeddings from a reasonable encoder do not trigger this.

## Defensive - MDBX_WRITEMAP aliasing in getDeletedIds

- **Issue**: under `MDBX_WRITEMAP`, `getDeletedIds` previously passed `mdbx_put` a pointer that came from a prior `mdbx_get` value buffer - i.e. it aliased the very page MDBX was about to overwrite. Documented MDBX UB.
- **Did not** cause Bug B's seq=288 corruption (the dedup fix alone removed the duplicate), but the code was UB regardless.
- **Fix**: `src/storage/id_mapper.hpp` `getDeletedIds` copies the remainder into a caller-owned `std::vector<idInt>` before writing back:

  ```cpp
  std::vector<idInt> remainder(raw + count, raw + total);
  MDBX_val new_val;
  new_val.iov_len = remainder.size() * sizeof(idInt);
  new_val.iov_base = remainder.data();
  mdbx_put(txn, dbi_, &key, &new_val, MDBX_UPSERT);
  ```

## Validation

Full sweep after all fixes: 6 sequential seeds x 5 cycles x 100 ops, concurrent 4-writer with and without deletes x 5 cycles x 2s, xenv concurrent x 12 cycles, plus `ctest --output-on-failure`. All pass.
