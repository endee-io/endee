"""
Smoke test: Endee must be running (e.g. docker compose up -d endee).
Run: python scripts/verify_endee.py
"""
from __future__ import annotations

import os
import sys

# Allow running from backend/ or repo root
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from endee import Endee, Precision


def main() -> int:
    base = os.environ.get("ENDEE_BASE_URL", "http://127.0.0.1:8080/api/v1")
    token = os.environ.get("ENDEE_AUTH_TOKEN") or None
    client = Endee(token=token)
    client.set_base_url(base)

    name = "verify_smoke"
    dim = 8
    try:
        client.create_index(
            name=name,
            dimension=dim,
            space_type="cosine",
            precision=Precision.INT8,
        )
    except Exception as e:
        err = str(e).lower()
        if "already" in err or "exist" in err or "duplicate" in err:
            pass
        else:
            print("create_index:", e)
            return 1

    index = client.get_index(name=name)
    v = [0.1] * dim
    index.upsert(
        [
            {"id": "smoke_1", "vector": v, "meta": {"t": "a"}},
            {"id": "smoke_2", "vector": [0.2] * dim, "meta": {"t": "b"}},
        ]
    )
    results = index.query(vector=v, top_k=2)
    print("OK query returned", len(results), "hits")
    for r in results:
        print(" ", r.get("id"), r.get("similarity"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
