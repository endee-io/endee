#!/usr/bin/env python3
"""
Test 01: WAL is atomic with data — there is no separate WAL file.

This test corresponds to "Attempt 1: Naive flush() to ofstream" in
docs/op_log_vs_wal.md. The structural property it asserts is the precondition
for every failure mode in that document: if the WAL lives in its own file,
outside MDBX, then it can disagree with MDBX across a crash. The op_log
design eliminates the disagreement by putting the log inside MDBX, which
means there is NO separate WAL file to be found on disk.

This is a deterministic, single-process observation. No crash needed.

  Expected on `master`:      FAIL  (a `wal.bin` file exists in the index dir)
  Expected on `single_txn`:  PASS  (no `wal.bin`; the log lives in mdbx.dat)
"""

import sys
from pathlib import Path

from harness import Server, TestReport


def find_separate_wal_files(data_dir: Path) -> list[Path]:
    """Anything under the data dir that looks like a standalone WAL file."""
    matches = []
    for path in data_dir.rglob("*"):
        if not path.is_file():
            continue
        name = path.name.lower()
        if name in ("wal.bin", "wal.log") or name.endswith(".wal"):
            matches.append(path)
    return matches


def main() -> int:
    report = TestReport(Path(__file__).name)
    server = Server()
    try:
        server.start()
        server.create_index("t", dim=2)
        server.insert("t", "v_X", [1.0, 0.0])
        server.insert("t", "v_Y", [0.0, 1.0])
        server.stop()  # graceful shutdown; flushes everything

        wal_files = find_separate_wal_files(server.data_dir)
        report.expect(
            len(wal_files) == 0,
            "Found a separate WAL file outside MDBX after basic operations. "
            "Atomic commit across MDBX and a separate file is impossible in "
            "POSIX — see docs/op_log_vs_wal.md. Offending files: "
            + ", ".join(str(p.relative_to(server.data_dir)) for p in wal_files),
        )
    finally:
        server.stop()
        server.cleanup()

    return report.finalize()


if __name__ == "__main__":
    sys.exit(main())
