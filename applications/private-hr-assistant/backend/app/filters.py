from __future__ import annotations


def attribute_filters(dept_code: str | None, doc_type: str | None) -> list[dict] | None:
    fl: list[dict] = []
    if dept_code:
        fl.append({"dept_code": {"$eq": dept_code}})
    if doc_type:
        fl.append({"doc_type": {"$eq": doc_type}})
    return fl or None
