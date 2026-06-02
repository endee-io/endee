"""
Crash harness for the single_txn branch.

Contract under test (from docs/mdbx_shared_env_acid_revamp.md):
  - A successful insert/delete HTTP 200 means the shared MDBX txn has committed
    both the data rows and the op_log row. fsync has happened.
  - On crash + restart, op_log replay rebuilds the HNSW graph from MDBX rows,
    so every ack'd op must be observable post-restart.
  - Anything that did NOT return 200 (in-progress at kill time) is allowed to be
    either fully present or fully absent - but never half-applied.

Workload:
  - Deterministic vector generation: vector_id="v_{seq:08d}", vector derived from seq.
  - Mix of inserts and per-id deletes.
  - Each cycle: start server -> verify prior survivors -> do N ops -> SIGKILL.
  - Optional concurrent kill: start a background writer, SIGKILL mid-write.

Failure modes the harness detects:
  - ACK'd insert is missing or returns wrong vector after restart (durability bug).
  - ACK'd delete reappears after restart (durability bug for deletes).
  - In-progress op leaves the index with a corrupt half-applied row (atomicity bug).
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import msgpack
import requests

DIM = 8
SPACE = "l2"
USER = "endee"
INDEX = "ch"


def deterministic_vector(seq: int) -> list[float]:
    # Independent per-coordinate hash so consecutive seqs DON'T share most dims.
    # The previous formula `(seq+i)*c % m` made dv(s) and dv(s+1) overlap on 7 of
    # 8 coordinates, which pushed HNSW into pathological tight-cluster territory
    # and made it impossible to tell apart "graph bug" from "low-effective-dim
    # workload". This generator gives each (seq, i) pair an independent value.
    out = []
    for i in range(DIM):
        h = (seq * 2654435761 + i * 40503 + 0x9E3779B1) & 0xFFFFFFFF
        h ^= (h >> 16)
        h = (h * 0x85EBCA6B) & 0xFFFFFFFF
        h ^= (h >> 13)
        h = (h * 0xC2B2AE35) & 0xFFFFFFFF
        h ^= (h >> 16)
        out.append(h / 0xFFFFFFFF)
    return out


def vector_id(seq: int) -> str:
    return f"v_{seq:08d}"


@dataclass
class ServerProc:
    binary: str
    data_dir: str
    port: int
    proc: Optional[subprocess.Popen] = None
    log_path: str = "/tmp/crash_harness_server.log"
    ld_preload: str = ""

    def start(self) -> None:
        env = os.environ.copy()
        env["NDD_DATA_DIR"] = self.data_dir
        env["NDD_SERVER_PORT"] = str(self.port)
        env["NDD_NUM_SERVER_THREADS"] = "2"
        if self.ld_preload:
            env["LD_PRELOAD"] = self.ld_preload
        log = open(self.log_path, "ab")
        log.write(f"\n=== server start at {time.time()} ===\n".encode())
        log.flush()
        self.proc = subprocess.Popen(
            [self.binary],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self._wait_ready()

    def _wait_ready(self, timeout_s: float = 30.0) -> None:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"server died during startup, exit={self.proc.returncode}. "
                    f"check {self.log_path}"
                )
            try:
                r = requests.get(f"http://127.0.0.1:{self.port}/api/v1/health", timeout=1.0)
                if r.status_code == 200:
                    return
            except requests.exceptions.RequestException:
                pass
            time.sleep(0.1)
        raise RuntimeError(f"server never came healthy within {timeout_s}s")

    def kill(self) -> None:
        if self.proc and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=10)
        self.proc = None

    def graceful_stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.kill(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        self.proc = None


class Client:
    def __init__(self, port: int):
        self.base = f"http://127.0.0.1:{port}/api/v1"
        self.s = requests.Session()

    def create_index(self) -> bool:
        r = self.s.post(
            f"{self.base}/index/create",
            json={"index_name": INDEX, "dim": DIM, "space_type": SPACE},
            timeout=10,
        )
        return r.status_code == 200

    def insert(self, seqs: list[int]) -> bool:
        body = [{"id": vector_id(s), "vector": deterministic_vector(s)} for s in seqs]
        r = self.s.post(
            f"{self.base}/index/{INDEX}/vector/insert",
            json=body,
            timeout=15,
        )
        return r.status_code == 200

    def delete(self, seq: int) -> Optional[bool]:
        """Return True if 200, False if 404, None on other error."""
        r = self.s.delete(
            f"{self.base}/index/{INDEX}/vector/{vector_id(seq)}/delete",
            timeout=15,
        )
        if r.status_code == 200:
            return True
        if r.status_code == 404:
            return False
        return None

    def get(self, seq: int) -> Optional[list[float]]:
        """Return decoded vector if present, None if 404. Raise on other errors.

        HybridVectorObject is msgpacked as a positional list
          [id, meta, filter, norm, vector, sparse_ids, sparse_values]
        See docs/followups.md for the rationale and the future-work note.
        """
        r = self.s.post(
            f"{self.base}/index/{INDEX}/vector/get",
            json={"id": vector_id(seq)},
            timeout=15,
        )
        if r.status_code == 404:
            return None
        if r.status_code != 200:
            raise RuntimeError(f"GET unexpected status {r.status_code}: {r.text[:200]}")
        obj = msgpack.unpackb(r.content, raw=False)
        if not isinstance(obj, list) or len(obj) < 5:
            raise RuntimeError(f"GET returned unexpected msgpack shape: {obj!r}")
        return obj[4]

    def search_ids(self, query_vec: list[float], k: int = 10, ef: int = 200) -> set[str]:
        """Search the index using query_vec, return the set of ids in the top-k.

        searchKNN returns std::vector<VectorResult> packed directly, so the wire
        format is a positional list of [similarity, id, meta, filter, norm, vector]
        tuples. The id is at index 1.
        """
        r = self.s.post(
            f"{self.base}/index/{INDEX}/search",
            json={"vector": query_vec, "k": k, "ef": ef},
            timeout=15,
        )
        if r.status_code != 200:
            raise RuntimeError(f"search unexpected status {r.status_code}: {r.text[:200]}")
        decoded = msgpack.unpackb(r.content, raw=False)
        if not isinstance(decoded, list):
            raise RuntimeError(f"search returned unexpected msgpack shape: {decoded!r}")
        ids = set()
        for row in decoded:
            if isinstance(row, list) and len(row) >= 2:
                ids.add(row[1])
        return ids


@dataclass
class State:
    """Shadow model of what the DB *must* contain.

    All mutations happen under `lock` so concurrent writer threads can share it.
    Single-threaded callers can ignore the lock (it's reentrant-safe with `with`).
    """
    next_seq: int = 0
    acked_present: dict[int, list[float]] = field(default_factory=dict)
    acked_absent: set[int] = field(default_factory=set)
    inflight: list[tuple[str, int]] = field(default_factory=list)
    lock: threading.Lock = field(default_factory=threading.Lock)

    def stats(self) -> str:
        with self.lock:
            return (
                f"next_seq={self.next_seq} "
                f"present={len(self.acked_present)} "
                f"absent={len(self.acked_absent)} "
                f"inflight={len(self.inflight)}"
            )

    def reserve_insert_seqs(self, n: int) -> list[int]:
        with self.lock:
            seqs = [self.next_seq + i for i in range(n)]
            self.next_seq += n
            for s in seqs:
                self.inflight.append(("insert", s))
            return seqs

    def commit_insert(self, seqs: list[int]) -> None:
        with self.lock:
            for s in seqs:
                self.acked_present[s] = deterministic_vector(s)
                try:
                    self.inflight.remove(("insert", s))
                except ValueError:
                    pass

    def fail_insert(self, seqs: list[int]) -> None:
        with self.lock:
            for s in seqs:
                try:
                    self.inflight.remove(("insert", s))
                except ValueError:
                    pass

    def pick_delete_target(self, rng: random.Random) -> Optional[int]:
        with self.lock:
            if not self.acked_present:
                return None
            seq = rng.choice(list(self.acked_present.keys()))
            self.inflight.append(("delete", seq))
            return seq

    def commit_delete(self, seq: int, was_present: bool) -> None:
        with self.lock:
            try:
                self.inflight.remove(("delete", seq))
            except ValueError:
                pass
            if was_present:
                self.acked_absent.add(seq)
                self.acked_present.pop(seq, None)

    def abandon_delete(self, seq: int) -> None:
        with self.lock:
            try:
                self.inflight.remove(("delete", seq))
            except ValueError:
                pass


VEC_EPS = 1e-3  # int16 quantization of values in [0,1] is precise to ~3e-5 per dim;
                # widen for safety against accumulated rounding.


def vectors_close(a: list[float], b: list[float]) -> bool:
    if len(a) != len(b):
        return False
    return all(abs(x - y) < VEC_EPS for x, y in zip(a, b))


def verify_durability(
    client: Client,
    state: State,
    ctx: str,
    rng: random.Random,
    search_sample: int = 200,
    search_k: int = 10,
) -> list[str]:
    """Check every ack'd-present is present with the right vector; every ack'd-absent is absent.

    Two verification surfaces:
      A. MDBX: exhaustive getVector for every acked_present / acked_absent seq.
      B. HNSW: sample-based search probe. For sampled acked_present, search with the
         vector and assert the id appears in top-k (HNSW recovery worked). For sampled
         acked_absent, search and assert the id does NOT appear (markDelete worked
         and survived).

    HNSW is approximate. With k=10 and ef=200 on a self-query, recall is effectively 1
    for the exact match; misses indicate HNSW recovery dropped a label, not approximation
    noise. We sample because exhaustive search at 150k+ seqs takes minutes per cycle.
    """
    failures: list[str] = []

    # --- A. MDBX side: exhaustive (this is fast - point reads) ---
    for seq, expected_vec in state.acked_present.items():
        got = client.get(seq)
        if got is None:
            failures.append(
                f"[{ctx}] DURABILITY (MDBX): acked insert seq={seq} id={vector_id(seq)} "
                f"is MISSING after restart"
            )
            continue
        if not vectors_close(got, expected_vec):
            failures.append(
                f"[{ctx}] CORRUPTION (MDBX): acked insert seq={seq} returned wrong vector: "
                f"expected={expected_vec} got={got}"
            )

    for seq in state.acked_absent:
        got = client.get(seq)
        if got is not None:
            failures.append(
                f"[{ctx}] DURABILITY (MDBX): acked delete seq={seq} id={vector_id(seq)} "
                f"REAPPEARED after restart (vector={got})"
            )

    for kind, seq in state.inflight:
        try:
            got = client.get(seq)
        except RuntimeError as e:
            failures.append(f"[{ctx}] inflight check {kind} seq={seq}: {e}")
            continue
        if got is not None:
            expected = deterministic_vector(seq)
            if not vectors_close(got, expected):
                failures.append(
                    f"[{ctx}] CORRUPTION (inflight): seq={seq} id={vector_id(seq)} "
                    f"present after restart with WRONG vector: expected={expected} got={got}"
                )

    # --- B. HNSW side: sample-based ---
    present_seqs = list(state.acked_present.keys())
    absent_seqs = list(state.acked_absent)
    sample_present = rng.sample(present_seqs, min(search_sample, len(present_seqs)))
    sample_absent = rng.sample(absent_seqs, min(search_sample, len(absent_seqs)))

    missing_in_hnsw: list[int] = []
    for seq in sample_present:
        try:
            ids = client.search_ids(deterministic_vector(seq), k=search_k)
        except RuntimeError as e:
            failures.append(f"[{ctx}] search probe (present) seq={seq}: {e}")
            continue
        if vector_id(seq) not in ids:
            missing_in_hnsw.append(seq)
    if missing_in_hnsw:
        # Up to 5 examples reported; full count in the message.
        sample_str = ", ".join(str(s) for s in missing_in_hnsw[:5])
        failures.append(
            f"[{ctx}] HNSW RECOVERY: {len(missing_in_hnsw)}/{len(sample_present)} sampled "
            f"acked-present seqs are MISSING from search top-{search_k} for self-query "
            f"(sample seqs: {sample_str})"
        )

    leaked_in_hnsw: list[int] = []
    for seq in sample_absent:
        try:
            ids = client.search_ids(deterministic_vector(seq), k=search_k)
        except RuntimeError as e:
            failures.append(f"[{ctx}] search probe (absent) seq={seq}: {e}")
            continue
        if vector_id(seq) in ids:
            leaked_in_hnsw.append(seq)
    if leaked_in_hnsw:
        sample_str = ", ".join(str(s) for s in leaked_in_hnsw[:5])
        failures.append(
            f"[{ctx}] HNSW DELETE: {len(leaked_in_hnsw)}/{len(sample_absent)} sampled "
            f"acked-deleted seqs are STILL APPEARING in search top-{search_k} "
            f"(sample seqs: {sample_str})"
        )

    return failures


def reconcile_inflight_after_restart(client: Client, state: State) -> None:
    """After a crash, inflight ops landed either fully or not at all.
    Re-probe each one and absorb the outcome into the shadow model so the next
    cycle's checks have a consistent baseline.
    """
    for kind, seq in state.inflight:
        got = client.get(seq)
        if kind == "insert":
            if got is not None:
                state.acked_present[seq] = deterministic_vector(seq)
            # else: stayed unwritten, which is fine
        elif kind == "delete":
            if got is None:
                state.acked_absent.add(seq)
                state.acked_present.pop(seq, None)
            else:
                # delete did not take effect; the insert before it should still be present
                pass
    state.inflight.clear()


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def workload_batch(
    client: Client,
    state: State,
    n_ops: int,
    rng: random.Random,
    delete_prob: float,
    batch_size: int = 10,
    stop: Optional[threading.Event] = None,
) -> None:
    """Drive n_ops sequential ops (insert/delete), updating state on each ack.

    `stop` lets a concurrent writer thread exit when the main thread is ready
    to kill the server. Sequential mode passes None.
    """
    ops_done = 0
    while ops_done < n_ops:
        if stop is not None and stop.is_set():
            return
        do_delete = rng.random() < delete_prob
        target = state.pick_delete_target(rng) if do_delete else None

        if target is not None:
            try:
                result = client.delete(target)
            except requests.exceptions.RequestException:
                state.abandon_delete(target)
                return
            if result is True:
                state.commit_delete(target, was_present=True)
            else:
                state.abandon_delete(target)
            ops_done += 1
        else:
            bsz = min(batch_size, n_ops - ops_done)
            seqs = state.reserve_insert_seqs(bsz)
            try:
                ok = client.insert(seqs)
            except requests.exceptions.RequestException:
                # connection broke mid-call - leave inflight, will be reconciled.
                return
            if ok:
                state.commit_insert(seqs)
            else:
                state.fail_insert(seqs)
            ops_done += bsz


def workload_concurrent(
    port: int,
    state: State,
    rng_seed: int,
    n_writers: int,
    duration_s: float,
    delete_prob: float,
) -> None:
    """Spawn n_writers writer threads that hammer the server for duration_s.

    Each writer has its own Client (own connection pool) and its own rng.
    State mutations go through the locked methods on State, so HTTP-200 acks
    are recorded atomically before the kill signal can land.
    """
    stop = threading.Event()
    threads: list[threading.Thread] = []
    for i in range(n_writers):
        c = Client(port)
        r = random.Random(rng_seed * 1000003 + i)
        t = threading.Thread(
            target=workload_batch,
            args=(c, state, 10**9, r, delete_prob),
            kwargs={"stop": stop},
            daemon=True,
        )
        t.start()
        threads.append(t)

    time.sleep(duration_s)
    stop.set()
    for t in threads:
        t.join(timeout=5.0)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--data-dir", default="/tmp/crash_harness_run")
    p.add_argument("--port", type=int, default=0, help="0 = auto-pick free port")
    p.add_argument("--cycles", type=int, default=20)
    p.add_argument("--ops-per-cycle", type=int, default=200,
                   help="if set, every cycle is this many ops (overrides min/max)")
    p.add_argument("--cycle-ops-min", type=int, default=0,
                   help="when nonzero, pick cycle size uniformly in [min, max]")
    p.add_argument("--cycle-ops-max", type=int, default=0)
    p.add_argument("--mode", choices=["sequential", "concurrent"], default="sequential")
    p.add_argument("--writers", type=int, default=4,
                   help="concurrent mode: number of writer threads")
    p.add_argument("--cycle-seconds", type=float, default=2.0,
                   help="concurrent mode: seconds writers run before kill")
    p.add_argument("--delete-prob", type=float, default=0.2)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--server-ld-preload", default="",
                   help="LD_PRELOAD value passed to the server process (e.g. fsync shim)")
    p.add_argument("--keep-data", action="store_true",
                   help="don't wipe data dir at start")
    args = p.parse_args()

    rng = random.Random(args.seed)
    port = args.port if args.port else free_port()

    if not args.keep_data and os.path.exists(args.data_dir):
        shutil.rmtree(args.data_dir)
    os.makedirs(args.data_dir, exist_ok=True)

    state = State()
    server = ServerProc(args.binary, args.data_dir, port, ld_preload=args.server_ld_preload)

    print(f"=== crash harness ===")
    print(f"  binary    {args.binary}")
    print(f"  data_dir  {args.data_dir}")
    print(f"  port      {port}")
    print(f"  cycles    {args.cycles}")
    print(f"  mode      {args.mode}")
    if args.mode == "sequential":
        if args.cycle_ops_min and args.cycle_ops_max:
            print(f"  ops/cycle [{args.cycle_ops_min}, {args.cycle_ops_max}] (random)")
        else:
            print(f"  ops/cycle {args.ops_per_cycle}")
    else:
        print(f"  writers   {args.writers}")
        print(f"  cycle_s   {args.cycle_seconds}")
    print(f"  seed      {args.seed}")
    if args.server_ld_preload:
        print(f"  LD_PRELOAD {args.server_ld_preload}")
    print()

    total_failures: list[str] = []

    # Initial start: create index.
    server.start()
    client = Client(port)
    if not client.create_index():
        print("FATAL: failed to create index on initial start", file=sys.stderr)
        server.kill()
        return 2

    for cycle in range(args.cycles):
        if args.mode == "sequential":
            if args.cycle_ops_min and args.cycle_ops_max:
                n_ops = rng.randint(args.cycle_ops_min, args.cycle_ops_max)
            else:
                n_ops = args.ops_per_cycle
            workload_batch(client, state, n_ops, rng, args.delete_prob)
        else:  # concurrent
            workload_concurrent(
                port=port,
                state=state,
                rng_seed=args.seed + cycle,
                n_writers=args.writers,
                duration_s=args.cycle_seconds,
                delete_prob=args.delete_prob,
            )

        # Kill the server hard.
        server.kill()

        # Restart on same data dir.
        server.start()
        client = Client(port)

        # Reconcile any inflight ops (insert/delete partially observed): either it
        # landed durably or it didn't.
        reconcile_inflight_after_restart(client, state)

        # Now invariant: everything in acked_present must come back, acked_absent must stay gone.
        ctx = f"cycle={cycle}"
        failures = verify_durability(client, state, ctx, rng)
        if failures:
            total_failures.extend(failures)
            for f in failures[:5]:
                print("FAIL:", f)
            if len(failures) > 5:
                print(f"  ... and {len(failures) - 5} more this cycle")
        print(f"[cycle {cycle:>3}] {state.stats()} failures_so_far={len(total_failures)}")

    # Final graceful shutdown.
    server.graceful_stop()

    print()
    print(f"=== done ===")
    print(f"total operations issued (~): {state.next_seq + len(state.acked_absent)}")
    print(f"total cycles:                {args.cycles}")
    print(f"total failures:              {len(total_failures)}")

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
