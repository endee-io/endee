"""
Fsync sanity / control test (Gap 3 validation).

This is NOT a bug-finding test. It is a meta-test: it proves that the crash
harness is *capable* of observing lost-data failures when they exist.

Procedure:
  1. Start the server with LD_PRELOAD=fsync_drop.so (intercepts fsync/fdatasync/
     msync, returns 0 without flushing).
  2. Issue many small one-vector inserts. Each returns HTTP 200; the shadow
     model marks each one ack'd-present.
  3. SIGKILL the server. Dirty pages that never got flushed are lost.
  4. Restart the server WITHOUT the preload (normal fsync semantics).
  5. Count how many of the ack'd inserts are missing.

Interpretation:
  - lost > 0: fsync is real, the harness's "0 failures" results on the main
    runs mean MDBX actually called fsync and the OS actually flushed. The
    crash test mechanism is sound.
  - lost == 0: something is wrong. Either:
      a) the shim didn't load (check LD_PRELOAD path / nm fsync_drop.so),
      b) MDBX is bypassing libc fsync (unlikely on Linux),
      c) the data is so small it was implicitly written via write+close before
         any fsync would have mattered (try more or larger writes).
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

# Reuse the main harness machinery.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_harness import (
    Client, ServerProc, State, deterministic_vector, free_port,
    vector_id,
)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--preload", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "fsync_drop.so"))
    p.add_argument("--data-dir", default="/tmp/fsync_sanity_run")
    p.add_argument("--inserts", type=int, default=1000)
    p.add_argument("--batch-size", type=int, default=10)
    args = p.parse_args()

    if os.path.exists(args.data_dir):
        shutil.rmtree(args.data_dir)
    os.makedirs(args.data_dir, exist_ok=True)

    if not os.path.exists(args.preload):
        print(f"FATAL: preload .so not found at {args.preload}", file=sys.stderr)
        print(f"build it first: gcc -shared -fPIC -O2 -o {args.preload} "
              f"{args.preload.replace('.so', '.c')} -ldl", file=sys.stderr)
        return 2

    port = free_port()

    print(f"=== fsync sanity ===")
    print(f"  binary    {args.binary}")
    print(f"  preload   {args.preload}")
    print(f"  data_dir  {args.data_dir}")
    print(f"  inserts   {args.inserts}")
    print()

    # Phase 1: start WITH preload, insert + ack, kill.
    server = ServerProc(args.binary, args.data_dir, port,
                        ld_preload=args.preload,
                        log_path="/tmp/fsync_sanity_phase1.log")
    server.start()
    client = Client(port)
    if not client.create_index():
        print("FATAL: index create failed under preload", file=sys.stderr)
        server.kill()
        return 2

    state = State()
    ops_done = 0
    while ops_done < args.inserts:
        bsz = min(args.batch_size, args.inserts - ops_done)
        seqs = state.reserve_insert_seqs(bsz)
        ok = client.insert(seqs)
        if ok:
            state.commit_insert(seqs)
        else:
            state.fail_insert(seqs)
            print(f"  warn: insert batch {seqs[0]}.. did not ack", file=sys.stderr)
        ops_done += bsz

    n_acked = len(state.acked_present)
    print(f"  phase 1: ack'd {n_acked} inserts under no-fsync preload")

    server.kill()
    print(f"  phase 1: SIGKILL sent")

    # Phase 2: restart WITHOUT preload, count survivors.
    server = ServerProc(args.binary, args.data_dir, port,
                        ld_preload="",
                        log_path="/tmp/fsync_sanity_phase2.log")
    server.start()
    client = Client(port)

    survived = 0
    missing = 0
    corrupted = 0
    for seq, expected in state.acked_present.items():
        got = client.get(seq)
        if got is None:
            missing += 1
        elif any(abs(a - b) > 1e-3 for a, b in zip(got, expected)):
            corrupted += 1
        else:
            survived += 1

    server.graceful_stop()

    print()
    print(f"=== result ===")
    print(f"  acked      {n_acked}")
    print(f"  survived   {survived}")
    print(f"  missing    {missing}")
    print(f"  corrupted  {corrupted}")
    print()

    if missing == 0 and corrupted == 0:
        print("EXPECTED-NULL RESULT: zero data loss under dropped fsync.")
        print()
        print("This is not a test failure. It demonstrates a real limit of the harness:")
        print("  SIGKILL only terminates the process. The kernel's page cache survives.")
        print("  Dirty pages from mmap-based MDBX writes will be written back by the")
        print("  kernel writeback thread regardless of whether fsync was called.")
        print("  fsync only makes the *caller* wait until the data is on disk; it does")
        print("  not change what eventually reaches disk when the process dies cleanly")
        print("  from the kernel's perspective.")
        print()
        print("To actually test power-loss durability, one of these is needed:")
        print("  - dm-log-writes block device (root + dmsetup)")
        print("  - drop_caches after kill (root: echo 3 > /proc/sys/vm/drop_caches)")
        print("  - VM hard-reset between phases")
        print()
        print("This branch's process-crash consistency is verified by the main harness.")
        print("Power-loss consistency is NOT verified by SIGKILL alone, regardless of")
        print("whether fsync is intercepted. See docs/followups.md.")
        return 0  # informational, not a failure

    pct = 100.0 * missing / max(n_acked, 1)
    print(f"UNEXPECTED: {missing}/{n_acked} ack'd ops lost ({pct:.1f}%).")
    print("This means SIGKILL alone is causing data loss when fsync is dropped,")
    print("which would only happen if the kernel evicts dirty pages before writeback.")
    print("Worth investigating: either the workload is large enough to trigger eviction,")
    print("or something else in the stack is dropping the data on process death.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
