from __future__ import annotations

from functools import lru_cache

from endee import Endee, Precision

from app.config import get_settings


def _index_names(client: Endee) -> set[str]:
    raw = client.list_indexes() or []
    names: set[str] = set()
    for item in raw:
        if isinstance(item, dict):
            n = item.get("name") or item.get("index_name")
            if n:
                names.add(str(n))
        elif isinstance(item, str):
            names.add(item)
    return names


def build_endee_client() -> Endee:
    s = get_settings()
    token = s.endee_auth_token if s.endee_auth_token else None
    client = Endee(token=token)
    client.set_base_url(s.endee_base_url)
    return client


@lru_cache
def _cached_client() -> Endee:
    return build_endee_client()


def get_endee() -> Endee:
    """Fresh config in tests: clear _cached_client cache if needed."""
    return _cached_client()


def ensure_hr_index() -> None:
    s = get_settings()
    client = get_endee()
    if s.endee_index_name in _index_names(client):
        return
    client.create_index(
        name=s.endee_index_name,
        dimension=s.embedding_dimension,
        space_type="cosine",
        precision=Precision.INT8,
    )


def get_hr_index():
    ensure_hr_index()
    return get_endee().get_index(name=get_settings().endee_index_name)


def invalidate_endee_cache() -> None:
    _cached_client.cache_clear()
