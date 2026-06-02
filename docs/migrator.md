# `ndd-migrate-v0-to-v2`

Offline tool that rewrites legacy (layout v0, pre-`single_txn`) indexes into the current shared-MDBX layout (v2). The server has been stripped of legacy migration code; this binary is the only thing that can convert v0 data.

## When you need it

- The server refuses to load an index with `LEGACY_INDEX_LAYOUT_ERROR`.
- `restoreBackup` rejects a backup tar with the same error and an explicit pointer to this binary.
- An `info`/`load` path reports a `migration.inprogress` marker on disk - a previous migration crashed mid-flight and the partial output is not safe to use; re-run the migrator to overwrite it.

## Build

- Build with the rest of the tree: `cmake --build build -j` produces `build/ndd-migrate-v0-to-v2`.
- Convenience wrapper: `bash src/tools/build_migrator.sh`.

## Subcommands

### `in-place` - convert a live index folder

```
ndd-migrate-v0-to-v2 in-place \
    --data-dir <root>           # the server's data root (parent of <user>/<name>)
    --index-id <user>/<name>    # catalog id of the index to migrate
```

- The server must be stopped. The migrator opens the global catalog (`<data-dir>/meta/`) directly with no coordination.
- Rewrites `<data-dir>/<index-id>/` in place into the v2 shared-env layout.
- Bumps the catalog row's `layout_version` to the current value so the server loads the index natively on next start.
- Fails (refuses) if the catalog has no row for `<index-id>` - re-register the index first or use `from-backup` instead.

### `from-backup` - convert a backup tar

```
ndd-migrate-v0-to-v2 from-backup --backup <tar>
    ( --out-dir <dir> [--out-tar <new.tar>]
    | --replace-original [--out-dir <dir>] )
```

Two mutually-exclusive modes:

- **`--out-dir <dir>` (artifact mode)** - extracts `<tar>`, walks the single top-level folder, and migrates into `<dir>`. Optionally re-tars the result with `--out-tar <new.tar>`. The catalog is **not** touched; the operator decides where to drop the resulting folder/tar (e.g. feed `<new.tar>` to `restoreBackup`, or copy `<dir>` under `<data-dir>/<user>/`).
- **`--replace-original` (in-place tar swap)** - see semantics below. `--out-dir` becomes optional in this mode; if omitted, the migrator uses an internal scratch directory and cleans it up.
- `--replace-original` and `--out-tar` are mutually exclusive.
- Without `--replace-original`, `--out-dir` is required (the migrated folder is the entire artifact).

### `--replace-original` semantics

- Renames the input tar to `v0_<basename>` in the same directory (preserves the original).
- Writes the migrated tar to the original filename. The `backup.json` catalog keeps pointing at the original name with no edit needed.
- Crash-safe: writes to a sibling `.v2.partial`, then renames the original to `v0_<basename>`, then renames the `.v2.partial` into place. A crash between renames leaves `v0_<basename>` intact and a recoverable `.v2.partial`.
- Refuses to run if `v0_<basename>` already exists - rename or remove the previous safety copy before retrying.
- **Auto-scratch when `--out-dir` is omitted**: the migrator creates an absolute dot-prefixed sibling of the tar (e.g. `/backups/.b1.tar.migrating` next to `/backups/b1.tar`) as scratch space and removes it once the swap completes. Pass `--out-dir <dir>` if you want the unpacked migrated folder to stick around alongside the replaced tar.
- **Migrated tar's inner directory name is the tar's stem.** E.g. `b1.tar` produces entries under `b1/...`, not under the scratch-dir name. An operator who later runs `tar -xf b1.tar` sees a clean `b1/` directory.

## The `migration.inprogress` marker

- Written to the target directory before destructive work begins; removed on successful completion.
- The server (`loadIndex`, `getIndexInfo`, `VectorStorage` open) refuses any index folder that has this marker - see `settings::INDEX_MIGRATION_MARKER`.
- A retry of the migrator may overwrite a marker-bearing target. A completed target without the marker is left alone.

## What the migrator actually does

- Opens the legacy split MDBX envs (`vector_storage`, `meta`, `id_mapper`, `filter`, optional `sparse_storage`) `MDBX_RDONLY`.
- Creates a fresh v2 `SharedIndexEnv` (see `docs/mdbx_shared_env_acid_revamp.md` §Shared MDBX Layout) at `<target>/vectors`.
- Copies each legacy DBI into the matching named DBI inside the shared env.
- Generates an `op_log` of `VECTOR_ADD` rows so HNSW can be rebuilt idempotently on first load.
- Writes `layout_meta` so the next open sees a v2 environment.

## Verification

- `--help` (no subcommand) prints usage.
- Round-trip test: pick a small v0 backup tar, `from-backup --backup b.tar --out-dir tmp --out-tar b_v2.tar`, then point a fresh server at `b_v2.tar` via `restoreBackup` and confirm `searchKNN` returns the expected vectors. For the in-place swap variant, `from-backup --backup b.tar --replace-original` and verify the resulting `b.tar` opens cleanly under `restoreBackup`.
- The migrator is exercised by `tests/layout_migrator_test.cpp` in the main test binary - `ctest --test-dir build -R layout_migrator`.
