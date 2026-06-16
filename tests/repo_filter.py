#!/usr/bin/env python3
"""
Reproduce the duplicate-heavy numeric bucket cliff in endee.

Inserts 65,535 vectors with identical filter value, then 1 more (=65,536),
then 1 more (=65,537). Searches at each step. Recall collapses to 0 at the
65,536 boundary because Bucket::serialize truncates uint16_t count to 0.

Run with a fresh endee server on localhost:8080.
"""

import json
import os
import sys
import time
import requests
import msgpack

BASE = os.environ.get("ENDEE_URL", "http://localhost:8080")
TOKEN = os.environ.get("ENDEE_TOKEN", "")
INDEX = "bucket_repro"
DIM = 16
FILTER_VALUE = 100
BATCH = 1000

HEADERS = {"Content-Type": "application/json"}
if TOKEN:
    HEADERS["Authorization"] = f"Bearer {TOKEN}"


def post(path, payload):
    r = requests.post(f"{BASE}{path}", headers=HEADERS, data=json.dumps(payload))
    if r.status_code >= 400:
        raise RuntimeError(f"POST {path} -> {r.status_code}: {r.text}")
    return r


def delete(path):
    r = requests.delete(f"{BASE}{path}", headers=HEADERS)
    return r


def make_vector(i):
    # Deterministic 16-d vector that varies enough for HNSW to be meaningful
    return [((i + j * 7) % 1000) / 1000.0 for j in range(DIM)]


def search_filtered(k=10):
    body = {
        "vector": make_vector(0),
        "k": k,
        "filter": json.dumps([{"score": {"$eq": FILTER_VALUE}}]),
    }
    r = post(f"/api/v1/index/{INDEX}/search", body)
    results = msgpack.unpackb(r.content, raw=False)
    # results is List[VectorResult], each VectorResult is a list:
    # [similarity, id, meta, filter, norm, vector]
    return [row[1] for row in results]


def search_unfiltered(k=10):
    body = {"vector": make_vector(0), "k": k}
    r = post(f"/api/v1/index/{INDEX}/search", body)
    results = msgpack.unpackb(r.content, raw=False)
    return [row[1] for row in results]


def insert_range(lo, hi):
    """Insert ids v{lo} .. v{hi-1}."""
    for batch_start in range(lo, hi, BATCH):
        batch_end = min(batch_start + BATCH, hi)
        items = [
            {
                "id": f"v{i}",
                "vector": make_vector(i),
                "filter": json.dumps({"score": FILTER_VALUE}),
            }
            for i in range(batch_start, batch_end)
        ]
        post(f"/api/v1/index/{INDEX}/vector/insert", items)


def report(label, total_inserted):
    filtered = search_filtered(k=10)
    unfiltered = search_unfiltered(k=10)
    print(
        f"[{label}] inserted={total_inserted:>5}  "
        f"filtered_hits={len(filtered):>2}/10  "
        f"unfiltered_hits={len(unfiltered):>2}/10"
    )
    return len(filtered)


def main():
    # Cleanup any prior run
    delete(f"/api/v1/index/{INDEX}/delete")

    # 1. Create index
    post(
        "/api/v1/index/create",
        {
            "index_name": INDEX,
            "dim": DIM,
            "space_type": "l2",
            "precision": "float32",
            "sparse_model": "None",
        },
    )
    print(f"created index {INDEX} dim={DIM}")

    # 2. Insert 65,535 vectors
    t0 = time.time()
    insert_range(0, 65_535)
    print(f"inserted 65535 vectors in {time.time()-t0:.1f}s")

    # 3. Sanity: filtered search works
    pre_cliff_hits = report("pre-cliff ", 65_535)
    assert pre_cliff_hits == 10, "expected 10 filtered hits before cliff"

    # 4. The cliff: one more insert -> total 65,536
    insert_range(65_535, 65_536)
    cliff_hits = report("AT CLIFF  ", 65_536)

    # 5. Off the cliff: one more insert -> total 65,537
    insert_range(65_536, 65_537)
    post_cliff_hits = report("post-cliff", 65_537)

    # 6. Cleanup
    delete(f"/api/v1/index/{INDEX}/delete")

    # 7. Report
    print()
    print("=== Summary ===")
    print(f"pre-cliff (65,535)  filtered hits: {pre_cliff_hits}/10")
    print(f"AT cliff  (65,536)  filtered hits: {cliff_hits}/10  <-- expect 0")
    print(f"post cliff (65,537) filtered hits: {post_cliff_hits}/10")
    if cliff_hits == 0 and pre_cliff_hits > 0:
        print("BUG REPRODUCED: filter recall dropped to zero at the cliff.")
        sys.exit(0)
    print("BUG NOT REPRODUCED — investigate.")
    sys.exit(1)


if __name__ == "__main__":
    main()