#!/usr/bin/env python3
"""
Test 02: A phantom WAL entry on disk does not cause `search` and direct
`getVector` to disagree about the same vector.

This is the WAL-vs-MDBX disagreement from docs/op_log_vs_wal.md, framed
as an API-level invariant any vector DB must satisfy:

    "If a vector exists in storage (getVector returns it), search MUST
     also be able to return it. Storage and the search index must agree."

The branch-independent way to construct the precondition is to inject
a phantom VECTOR_DELETE record into the WAL file on disk while the
server is stopped. The injected record is 5 bytes -- exactly the
format master's WriteAheadLog::readEntries parses for VECTOR_DELETE
(master src/storage/wal.hpp:96-128). On master this triggers
recoverFromWAL → alg->markDelete(numeric_id), which removes the
vector from search results. The vector_storage row is untouched, so
getVector still returns the bytes.

The original Bug B scenario (phantom ADD → duplicate id in
DELETED_IDS_KEY) cannot be reproduced against master via the natural
API: master's deleteVectorsByIds (master src/core/ndd.hpp:1373-1421)
never removes the vector_storage row on delete, so recovery's
"vector_storage row missing -> reclaim numeric_id" branch never fires.
That is a separate master bug that masks Bug B; this test demonstrates
the more general WAL/MDBX disagreement that the missing-row bug masks.

  Expected on `master`:      FAIL  (a 5-byte phantom DELETE record can
                                    be appended to wal.bin while the
                                    server is stopped; on restart
                                    recovery markDeletes the vector and
                                    search no longer returns it, but
                                    getVector still does)
  Expected on `single_txn`:  PASS  (there is no separate WAL file to
                                    inject into; the op_log entry would
                                    have committed atomically with the
                                    data row, so the system has no
                                    on-disk representation of "an
                                    unapplied log entry waiting to be
                                    replayed")
"""

import struct
import sys
from pathlib import Path

from harness import Server, TestReport


def inject_phantom_delete_into_any_wal(data_dir: Path, numeric_id: int) -> list[Path]:
    """
    Append a fake VECTOR_DELETE(numeric_id) record to every standalone
    WAL file found in `data_dir`.

    Record layout matches both master's WriteAheadLog::log() (writer)
    and WriteAheadLog::readEntries() (reader) for VECTOR_DELETE -- the
    reader only consumes the extra [str_len][string_id] payload for
    VECTOR_ADD, not for VECTOR_DELETE:

        [op:u8 = 0x02 (VECTOR_DELETE)]
        [numeric_id:u32 little-endian]
    """
    record = struct.pack("<BI", 2, numeric_id)  # 5 bytes
    touched: list[Path] = []
    for path in data_dir.rglob("*"):
        if not path.is_file():
            continue
        name = path.name.lower()
        if name == "wal.bin" or name == "wal.log" or name.endswith(".wal"):
            with open(path, "ab") as f:
                f.write(record)
            touched.append(path)
    return touched


def search_returns_v_X(server: Server) -> bool:
    code, body = server.post_json(
        "/api/v1/index/t/search", {"vector": [1.0, 0.0], "k": 5}
    )
    if code != 200:
        return False
    # msgpack-encoded results array; v_X's string id will appear as the
    # bytes b"v_X" if and only if it is in the result set.
    return b"v_X" in body


def main() -> int:
    report = TestReport(Path(__file__).name)
    server = Server()
    try:
        # ── Phase 1: insert v_X. After this, v_X is in id_map → 1, in
        # vector_storage[1], and in HNSW as a live node.
        server.start()
        server.create_index("t", dim=2)
        server.insert("t", "v_X", [1.0, 0.0])
        server.stop()  # graceful shutdown; master clears its WAL on save

        # ── Phase 2: inject phantom VECTOR_DELETE(1). On single_txn
        # there is no wal.bin and the injection is a no-op. On master
        # each index has a wal.bin; the phantom is appended in a format
        # master's readEntries does correctly parse.
        touched = inject_phantom_delete_into_any_wal(server.data_dir, numeric_id=1)
        print(f"phantom injection: touched {len(touched)} WAL file(s): {touched}")

        # ── Phase 3: restart. On the first API call to the index,
        # recoverFromWAL fires. On master it replays the phantom DELETE,
        # markDeleting numeric_id 1 in HNSW. The vector_storage row is
        # left intact (master never deletes vector_storage rows; see
        # docstring), so getVector still returns the bytes -- but
        # search no longer does.
        server.start()
        find_in_search = search_returns_v_X(server)
        get_bytes = server.get_vector_bytes("t", "v_X")
        server.stop()

        report.expect(
            get_bytes is not None,
            "getVector('v_X') returned 404 -- vector_storage lost the row "
            "(setup is wrong; not the scenario this test asserts on)",
        )
        report.expect(
            find_in_search,
            "search([1.0, 0.0], k=5) did NOT return v_X, even though "
            "getVector('v_X') returns it. The injected phantom WAL "
            "DELETE markDelete'd numeric_id 1 in HNSW on restart, "
            "leaving the search index and the vector_storage row in "
            "permanent disagreement. The same injection is a no-op on "
            "single_txn because there is no separate WAL file to write to.",
        )
    finally:
        server.stop()
        server.cleanup()

    return report.finalize()


if __name__ == "__main__":
    sys.exit(main())
