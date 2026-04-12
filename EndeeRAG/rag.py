"""
RAG Module: Retrieval-Augmented Generation Pipeline
Combines retriever with LLM for context-aware answers with citations.
Includes conversation memory (WOW Feature #3).
"""
import sys
import time
import logging
from typing import List, Dict, Any, Optional
from collections import deque

from openai import OpenAI

from config import OPENAI_API_KEY, LLM_MODEL, LLM_MAX_TOKENS, LLM_TEMPERATURE
from retriever import HybridRetriever

# UTF-8 safe logging
logger = logging.getLogger("rag")
if not logger.handlers:
    _handler = logging.StreamHandler(
        open(sys.stdout.fileno(), mode="w", encoding="utf-8", closefd=False)
    )
    _handler.setFormatter(logging.Formatter("%(message)s"))
    logger.addHandler(_handler)
    logger.setLevel(logging.INFO)


class ConversationMemory:
    """Conversation memory for multi-turn interactions (WOW Feature #3)."""

    def __init__(self, max_turns: int = 10):
        self.history: deque = deque(maxlen=max_turns)
        self.max_turns = max_turns

    def add_turn(self, query: str, answer: str, sources: List[str] = None):
        self.history.append({
            "query": query,
            "answer": answer,
            "sources": sources or [],
            "timestamp": time.time(),
        })

    def get_context_string(self, last_n: int = 3) -> str:
        """Format recent conversation history as context."""
        if not self.history:
            return ""

        turns = list(self.history)[-last_n:]
        context_parts = ["### Previous Conversation:"]
        for i, turn in enumerate(turns, 1):
            context_parts.append(f"**Q{i}:** {turn['query']}")
            context_parts.append(f"**A{i}:** {turn['answer'][:500]}")

        return "\n".join(context_parts)

    def clear(self):
        self.history.clear()

    @property
    def turn_count(self) -> int:
        return len(self.history)


class RAGPipeline:
    """
    Complete RAG pipeline:
    - Retrieves relevant context from Endee via hybrid search
    - Constructs prompts with citations
    - Generates answers using Google Gemini
    - Maintains conversation memory
    """

    def __init__(self):
        logger.info("[RAG] Initializing pipeline...")
        self.retriever = HybridRetriever()
        self.memory = ConversationMemory()

        # Initialize OpenAI
        if OPENAI_API_KEY:
            self.llm = OpenAI(api_key=OPENAI_API_KEY)
            self.llm_available = True
            logger.info(f"[RAG] LLM: {LLM_MODEL} ready.")
        else:
            self.llm = None
            self.llm_available = False
            logger.info("[RAG] No OPENAI_API_KEY set. LLM generation disabled.")
            logger.info("[RAG] Set OPENAI_API_KEY in .env to enable AI-powered answers.")

        logger.info("[RAG] Pipeline ready.")

    def _build_context(self, results: List[Dict]) -> str:
        """Build context string from retrieved chunks with source attribution."""
        if not results:
            return "No relevant context found."

        context_parts = []
        for i, r in enumerate(results, 1):
            source = f"{r.get('filename', 'unknown')}"
            pages = r.get('source_pages', '[]')
            context_parts.append(
                f"[Source {i}: {source}, pages: {pages}]\n{r['text']}\n"
            )

        return "\n---\n".join(context_parts)

    def _build_fallback_answer(self, query: str, results: List[Dict]) -> str:
        """Build a nicely formatted answer from retrieved chunks when LLM is unavailable."""
        if not results:
            return "No relevant information was found in the uploaded documents for your query."

        parts = [
            "📚 **Retrieval Mode** — Showing the most relevant passages from your documents:\n"
        ]
        for i, r in enumerate(results, 1):
            source = r.get("filename", "unknown")
            pages = r.get("source_pages", "[]")
            similarity = r.get("similarity", 0)
            text = r.get("text", "").strip()
            # Truncate long chunks for readability
            if len(text) > 400:
                text = text[:400] + "..."
            parts.append(
                f"**[Source {i}]** *{source}* · Pages: {pages} · Score: {similarity:.4f}\n\n"
                f"> {text}\n"
            )

        parts.append(
            "\n---\n*💡 LLM generation is currently unavailable. "
            "The above passages were retrieved via hybrid search and are the most relevant to your question.*"
        )
        return "\n".join(parts)

    def _build_prompt(self, query: str, context: str,
                      conversation_context: str = "") -> str:
        """Build the LLM prompt with context, query, and conversation history."""
        prompt = f"""You are an intelligent document assistant. Answer the user's question based ONLY on the provided context. If the context doesn't contain enough information, say so clearly.

### Rules:
1. Answer based ONLY on the provided context
2. Cite your sources using [Source N] notation
3. If multiple sources support an answer, cite all of them
4. If the answer is not in the context, say "I don't have enough information in the provided documents to answer this question."
5. Be concise but thorough
6. Use markdown formatting for readability

{conversation_context}

### Retrieved Context:
{context}

### User Question:
{query}

### Answer:"""
        return prompt

    def query(self, query: str, mode: str = "hybrid", top_k: int = 5,
              filters: Optional[List[Dict]] = None,
              use_memory: bool = True) -> Dict[str, Any]:
        """
        Execute the full RAG pipeline:
        1. Retrieve relevant chunks
        2. Build context with citations
        3. Generate answer via LLM
        """
        # Guard: empty query
        if not query or not query.strip():
            return {
                "query": query or "",
                "answer": "Please enter a question to search your documents.",
                "citations": [],
                "search_mode": mode,
                "retrieval_time_ms": 0,
                "generation_time_ms": 0,
                "total_time_ms": 0,
                "chunks_retrieved": 0,
                "conversation_turn": self.memory.turn_count,
            }

        total_start = time.time()

        # Step 1: Retrieve
        try:
            search_result = self.retriever.search(query, mode=mode, top_k=top_k, filters=filters)
            retrieval_time = search_result["latency_ms"]
            results = search_result["results"]
        except Exception as e:
            logger.error(f"[RAG] Retrieval failed: {e}")
            return {
                "query": query,
                "answer": f"Retrieval error: {e}. Please try again.",
                "citations": [],
                "search_mode": mode,
                "retrieval_time_ms": 0,
                "generation_time_ms": 0,
                "total_time_ms": (time.time() - total_start) * 1000,
                "chunks_retrieved": 0,
                "conversation_turn": self.memory.turn_count,
            }

        # Step 2: Build context
        context = self._build_context(results)
        conversation_context = self.memory.get_context_string() if use_memory else ""

        # Step 3: Generate answer
        if self.llm_available:
            prompt = self._build_prompt(query, context, conversation_context)
            try:
                t0 = time.time()
                response = self.llm.chat.completions.create(
                    model=LLM_MODEL,
                    messages=[
                        {"role": "system", "content": "You are a helpful assistant."},
                        {"role": "user", "content": prompt}
                    ],
                    max_tokens=LLM_MAX_TOKENS,
                    temperature=LLM_TEMPERATURE,
                )
                generation_time = (time.time() - t0) * 1000
                answer = response.choices[0].message.content
            except Exception as e:
                logger.warning(f"[RAG] LLM generation failed: {e}")
                generation_time = 0
                answer = self._build_fallback_answer(query, results)
        else:
            generation_time = 0
            answer = self._build_fallback_answer(query, results)

        total_time = (time.time() - total_start) * 1000

        # Build citations
        citations = []
        for i, r in enumerate(results, 1):
            citations.append({
                "source_id": i,
                "chunk_id": r["id"],
                "filename": r.get("filename", "unknown"),
                "title": r.get("title", "Untitled"),
                "pages": r.get("source_pages", "[]"),
                "similarity": r["similarity"],
                "preview": r["text"][:200] + "..." if len(r["text"]) > 200 else r["text"],
            })

        # Update conversation memory
        if use_memory:
            source_names = [c["filename"] for c in citations]
            self.memory.add_turn(query, answer, source_names)

        return {
            "query": query,
            "answer": answer,
            "citations": citations,
            "search_mode": mode,
            "retrieval_time_ms": retrieval_time,
            "generation_time_ms": generation_time,
            "total_time_ms": total_time,
            "chunks_retrieved": len(results),
            "conversation_turn": self.memory.turn_count,
        }

    def query_with_document_filter(self, query: str, filename: str,
                                    mode: str = "hybrid", top_k: int = 5) -> Dict[str, Any]:
        """Query with document-specific filtering."""
        filters = [{"filename": {"$eq": filename}}]
        return self.query(query, mode=mode, top_k=top_k, filters=filters)

    def query_multi_document(self, query: str, filenames: List[str],
                              mode: str = "hybrid", top_k: int = 5) -> Dict[str, Any]:
        """Query across multiple specific documents."""
        filters = [{"filename": {"$in": filenames}}]
        return self.query(query, mode=mode, top_k=top_k, filters=filters)

    def clear_memory(self):
        """Clear conversation history."""
        self.memory.clear()


if __name__ == "__main__":
    pipeline = RAGPipeline()

    query = "What is this document about?"
    print(f"\n🧠 Query: {query}")
    result = pipeline.query(query)
    print(f"\n📝 Answer:\n{result['answer']}")
    print(f"\n📊 Stats:")
    print(f"  Retrieval: {result['retrieval_time_ms']:.0f}ms")
    print(f"  Generation: {result['generation_time_ms']:.0f}ms")
    print(f"  Total: {result['total_time_ms']:.0f}ms")
    print(f"\n📖 Citations:")
    for c in result["citations"]:
        print(f"  [Source {c['source_id']}] {c['filename']} (similarity: {c['similarity']:.4f})")
