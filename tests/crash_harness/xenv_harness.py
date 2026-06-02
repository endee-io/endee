"""
Cross-environment atomicity harness (Gap 4).

The single-txn revamp atomically commits within one index's shared MDBX environment,
but createIndex / deleteIndex / restoreBackup also touch the *global metadata catalog*
which is a separate MDBX env. The doc (docs/mdbx_shared_env_acid_revamp.md, "Global
Metadata Atomicity") explicitly notes:

  - createIndex: creates per-index env + initial files, then stores global metadata.
    A failure between these can leave an orphan directory or a catalog row pointing
    at an incomplete index.
  - deleteIndex: deletes the global metadata row and tombstones the per-index dir.
    A failure between can leave metadata and on-disk state disagreeing.

What this harness verifies after a SIGKILL + restart:
  1. Every "fully created" index (HTTP 200 from create) is listable via
     /api/v1/index/list and has a working info endpoint and survives writes.
  2. Every "fully deleted" index (HTTP 200 from delete) is NOT listable and
     no on-disk artifacts remain that confuse load.
  3. Half-state detection: list catalog vs ls data_dir/<user>/ and report any
     name that exists in one but not the other.

Acceptable on-disk states for a name with no ack'd delete:
  - listed in catalog + dir exists                     (committed create)
  - not in catalog + no dir                            (create never ack'd, fully aborted)
  - in catalog + dir exists + dir has migration marker (legacy migration path)
The unacceptable states are mismatches: catalog without dir, or dir without catalog.
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import requests

# Reuse the server-control plumbing from the main harness.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_harness import ServerProc, free_port, deterministic_vector, vector_id, DIM, SPACE, USER


def index_path(data_dir: str, index_name: str) -> str:
    return os.path.join(data_dir, USER, index_name)


class XClient:
    """Thin HTTP client for cross-env workload."""

    def __init__(self, port: int):
        self.base = f"http://127.0.0.1:{port}/api/v1"
        self.s = requests.Session()

    def create_index(self, name: str) -> bool:
        r = self.s.post(
            f"{self.base}/index/create",
            json={"index_name": name, "dim": DIM, "space_type": SPACE},
            timeout=10,
        )
        return r.status_code == 200

    def delete_index(self, name: str) -> Optional[bool]:
        r = self.s.delete(f"{self.base}/index/{name}/delete", timeout=10)
        if r.status_code == 200:
            return True
        if r.status_code == 404:
            return False
        return None

    def list_indexes(self) -> list[str]:
        r = self.s.get(f"{self.base}/index/list", timeout=10)
        r.raise_for_status()
        body = r.json()
        return [item["name"] for item in body.get("indexes", [])]

    def info(self, name: str) -> Optional[dict]:
        r = self.s.get(f"{self.base}/index/{name}/info", timeout=10)
        if r.status_code != 200:
            return None
        try:
            return r.json()
        except Exception:
            return None

    def insert_one(self, name: str, seq: int) -> bool:
        r = self.s.post(
            f"{self.base}/index/{name}/vector/insert",
            json=[{"id": vector_id(seq), "vector": deterministic_vector(seq)}],
            timeout=15,
        )
        return r.status_code == 200


@dataclass
class XState:
    # Names whose create returned 200 and whose subsequent delete (if any) did NOT return 200.
    expected_present: set[str] = field(default_factory=set)
    # Names whose delete returned 200 (whether or not they had been created on this run).
    expected_absent: set[str] = field(default_factory=set)
    # Ops that didn't observe a clean 2xx/4xx ack - outcome is either-or after restart.
    inflight: list[tuple[str, str]] = field(default_factory=list)  # ("create"|"delete", name)
    lock: threading.Lock = field(default_factory=threading.Lock)

    # locked helpers for concurrent mode
    def reserve(self, kind: str, name: str) -> None:
        with self.lock:
            self.inflight.append((kind, name))

    def commit_create(self, name: str) -> None:
        with self.lock:
            try: self.inflight.remove(("create", name))
            except ValueError: pass
            self.expected_present.add(name)

    def abort_create(self, name: str) -> None:
        with self.lock:
            try: self.inflight.remove(("create", name))
            except ValueError: pass

    def commit_delete(self, name: str) -> None:
        with self.lock:
            try: self.inflight.remove(("delete", name))
            except ValueError: pass
            self.expected_present.discard(name)
            self.expected_absent.add(name)

    def abort_delete(self, name: str) -> None:
        with self.lock:
            try: self.inflight.remove(("delete", name))
            except ValueError: pass

    def snapshot_present(self) -> list[str]:
        with self.lock:
            return list(self.expected_present)

    def has_seen(self, name: str) -> bool:
        with self.lock:
            return name in self.expected_present or name in self.expected_absent


def list_on_disk(data_dir: str) -> set[str]:
    user_dir = os.path.join(data_dir, USER)
    if not os.path.isdir(user_dir):
        return set()
    return {n for n in os.listdir(user_dir) if os.path.isdir(os.path.join(user_dir, n))}


def verify(client: XClient, state: XState, data_dir: str, ctx: str) -> list[str]:
    failures: list[str] = []
    listed = set(client.list_indexes())
    on_disk = list_on_disk(data_dir)

    # 1. Every expected-present must be listed AND have a data dir AND be queryable.
    for name in state.expected_present:
        if name not in listed:
            failures.append(
                f"[{ctx}] DURABILITY: index '{name}' was ack'd-created but missing from catalog"
            )
        if name not in on_disk:
            failures.append(
                f"[{ctx}] DURABILITY: index '{name}' was ack'd-created but has no data dir"
            )
        if client.info(name) is None:
            failures.append(
                f"[{ctx}] DURABILITY: index '{name}' was ack'd-created but info endpoint fails"
            )

    # 2. Every expected-absent must NOT be listed.
    for name in state.expected_absent:
        if name in listed:
            failures.append(
                f"[{ctx}] DURABILITY: index '{name}' was ack'd-deleted but REAPPEARED in catalog"
            )

    # 3. Half-state: any name that's in catalog but not on disk, or on disk but not in catalog,
    #    is only acceptable if the operation was in-progress at kill time. We allow that.
    inflight_names = {name for (_, name) in state.inflight}
    half_in_catalog_not_disk = (listed - on_disk) - inflight_names
    half_on_disk_not_catalog = (on_disk - listed) - inflight_names
    # Also strip ack'd-absent names that have a leftover dir - those are inconsistent but
    # need to be reported as the deleteIndex failure mode in the doc.
    half_in_catalog_not_disk -= state.expected_absent
    half_on_disk_not_catalog_real_orphans = (
        half_on_disk_not_catalog - state.expected_present
    )
    for name in half_in_catalog_not_disk:
        failures.append(
            f"[{ctx}] HALF-STATE: '{name}' is listed in catalog but no data dir on disk"
        )
    for name in half_on_disk_not_catalog_real_orphans:
        failures.append(
            f"[{ctx}] HALF-STATE: '{name}' has a data dir but is not listed in catalog (orphan)"
        )

    # 4. Half-state on ack'd-absent: dir leftover after ack'd delete.
    for name in state.expected_absent & on_disk:
        failures.append(
            f"[{ctx}] HALF-STATE: '{name}' was ack'd-deleted but still has on-disk data dir"
        )

    return failures


def reconcile_inflight(client: XClient, state: XState, data_dir: str) -> None:
    """After a crash, any inflight create/delete either landed or didn't.
    Observe the post-restart truth and fold it into the model.
    """
    listed = set(client.list_indexes())
    on_disk = list_on_disk(data_dir)

    for kind, name in state.inflight:
        if kind == "create":
            if name in listed and name in on_disk:
                state.expected_present.add(name)
            else:
                # neither, or only partial - system treats as not-created; we permit it.
                pass
        elif kind == "delete":
            if name not in listed and name not in on_disk:
                state.expected_absent.add(name)
                state.expected_present.discard(name)
    state.inflight.clear()


def workload_step_concurrent(
    port: int, state: XState, rng: random.Random, stop: threading.Event
) -> None:
    """One writer thread: bash on create/delete/insert until stop is set."""
    client = XClient(port)
    while not stop.is_set():
        present = state.snapshot_present()
        actions = ["create"]
        if present:
            actions.extend(["delete", "insert"])
        action = rng.choice(actions)

        if action == "create":
            name = f"xi_{rng.randrange(10**12):012d}"
            if state.has_seen(name):
                continue
            state.reserve("create", name)
            try:
                ok = client.create_index(name)
            except requests.exceptions.RequestException:
                continue  # leave inflight; reconcile will absorb
            if ok:
                state.commit_create(name)
            else:
                state.abort_create(name)
        elif action == "delete":
            if not present:
                continue
            name = rng.choice(present)
            state.reserve("delete", name)
            try:
                result = client.delete_index(name)
            except requests.exceptions.RequestException:
                continue
            if result is True:
                state.commit_delete(name)
            else:
                state.abort_delete(name)
        elif action == "insert":
            if not present:
                continue
            name = rng.choice(present)
            try:
                client.insert_one(name, seq=rng.randrange(10**9))
            except requests.exceptions.RequestException:
                pass


def run_concurrent(port: int, state: XState, seed: int, n_writers: int,
                   duration_s: float) -> None:
    stop = threading.Event()
    threads: list[threading.Thread] = []
    for i in range(n_writers):
        r = random.Random(seed * 1000003 + i)
        t = threading.Thread(
            target=workload_step_concurrent,
            args=(port, state, r, stop),
            daemon=True,
        )
        t.start()
        threads.append(t)
    time.sleep(duration_s)
    stop.set()
    for t in threads:
        t.join(timeout=5.0)


def workload_step(client: XClient, state: XState, rng: random.Random) -> None:
    """Pick one of: create, delete, insert-into-existing. Mutate state on ack."""
    actions = ["create"]
    if state.expected_present:
        actions.extend(["delete", "insert"])
    action = rng.choice(actions)

    if action == "create":
        name = f"xi_{rng.randrange(10**9):09d}"
        if name in state.expected_present or name in state.expected_absent:
            return  # collision; skip this step
        state.inflight.append(("create", name))
        try:
            ok = client.create_index(name)
        except requests.exceptions.RequestException:
            return
        # Remove inflight marker before reading ok so the abort branch is also clean.
        try:
            state.inflight.remove(("create", name))
        except ValueError:
            pass
        if ok:
            state.expected_present.add(name)

    elif action == "delete":
        name = rng.choice(list(state.expected_present))
        state.inflight.append(("delete", name))
        try:
            result = client.delete_index(name)
        except requests.exceptions.RequestException:
            return
        try:
            state.inflight.remove(("delete", name))
        except ValueError:
            pass
        if result is True:
            state.expected_present.discard(name)
            state.expected_absent.add(name)

    elif action == "insert":
        # Smoke: insert one vector to force the per-index env to do real work.
        name = rng.choice(list(state.expected_present))
        try:
            client.insert_one(name, seq=rng.randrange(10**9))
        except requests.exceptions.RequestException:
            return


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--data-dir", default="/tmp/xenv_harness_run")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--cycles", type=int, default=10)
    p.add_argument("--ops-per-cycle", type=int, default=40)
    p.add_argument("--mode", choices=["sequential", "concurrent"], default="sequential")
    p.add_argument("--writers", type=int, default=4)
    p.add_argument("--cycle-seconds", type=float, default=1.5)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--keep-data", action="store_true")
    args = p.parse_args()

    rng = random.Random(args.seed)
    port = args.port if args.port else free_port()

    if not args.keep_data and os.path.exists(args.data_dir):
        shutil.rmtree(args.data_dir)
    os.makedirs(args.data_dir, exist_ok=True)

    state = XState()
    server = ServerProc(args.binary, args.data_dir, port,
                        log_path="/tmp/xenv_harness_server.log")

    print(f"=== cross-env crash harness ===")
    print(f"  binary    {args.binary}")
    print(f"  data_dir  {args.data_dir}")
    print(f"  port      {port}")
    print(f"  cycles    {args.cycles}")
    print(f"  ops/cycle {args.ops_per_cycle}")
    print(f"  seed      {args.seed}")
    print()

    total_failures: list[str] = []
    server.start()
    client = XClient(port)

    for cycle in range(args.cycles):
        if args.mode == "sequential":
            for _ in range(args.ops_per_cycle):
                workload_step(client, state, rng)
        else:
            run_concurrent(port, state, args.seed + cycle, args.writers,
                           args.cycle_seconds)

        server.kill()
        server.start()
        client = XClient(port)

        reconcile_inflight(client, state, args.data_dir)

        ctx = f"cycle={cycle}"
        failures = verify(client, state, args.data_dir, ctx)
        if failures:
            total_failures.extend(failures)
            for f in failures[:5]:
                print("FAIL:", f)
            if len(failures) > 5:
                print(f"  ... and {len(failures) - 5} more this cycle")

        print(f"[cycle {cycle:>3}] "
              f"present={len(state.expected_present)} "
              f"absent={len(state.expected_absent)} "
              f"on_disk={len(list_on_disk(args.data_dir))} "
              f"failures_so_far={len(total_failures)}")

    server.graceful_stop()

    print()
    print(f"=== done ===  failures={len(total_failures)}")
    if total_failures:
        print()
        print("=== first 20 failures ===")
        for f in total_failures[:20]:
            print(" ", f)
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
