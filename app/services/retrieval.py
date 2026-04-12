from typing import Dict, List


def retrieve_top_chunks(store, query_vector: List[float], top_k: int = 5) -> List[Dict[str, object]]:
    return store.search(query_vector, k=top_k)


def build_context(results: List[Dict[str, object]], max_chunks: int = 3) -> str:
    context_chunks = []
    for result in results[:max_chunks]:
        meta = result.get("meta", {})
        if isinstance(meta, dict):
            filename = meta.get("filename", "unknown")
            chunk_id = meta.get("chunk_id", "")
            text = meta.get("text", "")
            truncated_text = " ".join(text.split()[:280])
            context_chunks.append(f"Source: {filename} chunk {chunk_id}\n{truncated_text}")
        else:
            context_chunks.append(str(meta))
    return "\n\n".join(context_chunks)
