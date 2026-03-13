"""
RAG prompt templates.

These templates follow RAG best practices:
  - Ground the model in the provided context
  - Instruct it to decline when information is missing
  - Separate system instructions from user content
"""

# ── System prompt ──────────────────────────────────────────────────────

RAG_SYSTEM_PROMPT = (
    "You are a helpful assistant that answers questions using ONLY the "
    "provided context. Follow these rules strictly:\n\n"
    "1. Base your answer exclusively on the context below.\n"
    "2. If the context does not contain enough information to answer, "
    "say: \"I don't have enough information to answer that question.\"\n"
    "3. Cite the source numbers (e.g., [Source 1]) when referencing "
    "specific information.\n"
    "4. Be concise, accurate, and well-structured.\n"
    "5. Do not make up information or use knowledge outside the context."
)


# ── Prompt builder ─────────────────────────────────────────────────────

def build_rag_prompt(question: str, context: str) -> str:
    """
    Build the full RAG prompt combining context and question.

    Args:
        question: The user's question.
        context:  Formatted context from the retriever.

    Returns:
        A complete prompt string ready for the LLM.
    """
    return (
        f"CONTEXT:\n"
        f"========\n"
        f"{context}\n\n"
        f"QUESTION:\n"
        f"=========\n"
        f"{question}\n\n"
        f"ANSWER:"
    )
