from app.endee_client import EndeeManager
from app.embeddings import EmbeddingEngine
from app.document_processor import DocumentProcessor
from app.config import settings


class RAGPipeline:
    """
    Full RAG pipeline:
    1. Ingest: PDF/text → chunk → embed → store in Endee
    2. Search: query → embed → Endee similarity search → ranked results
    3. Query:  question → search → build context → LLM → grounded answer
    """

    def __init__(self):
        self.endee = EndeeManager(
            base_url=settings.endee_base_url,
            auth_token=settings.endee_auth_token
        )
        self.embedder = EmbeddingEngine()
        self.processor = DocumentProcessor()

    # ─── INGEST ─────────────────────────────────────────────────────────

    def ingest_text(self, text: str, doc_name: str) -> dict:
        """Ingest raw text: chunk → embed → upsert to Endee."""
        chunks = self.processor.process_text(text, doc_name)
        return self._embed_and_upsert(chunks, doc_name)

    def ingest_pdf(self, file_bytes: bytes, doc_name: str) -> dict:
        """Ingest PDF: extract → chunk → embed → upsert to Endee."""
        chunks = self.processor.process_pdf(file_bytes, doc_name)
        return self._embed_and_upsert(chunks, doc_name)

    def _embed_and_upsert(self, chunks: list[dict], doc_name: str) -> dict:
        if not chunks:
            return {"status": "error", "message": "No text extracted", "chunks_indexed": 0}

        texts = [c["text"] for c in chunks]
        vectors = self.embedder.embed_batch(texts)

        for chunk, vec in zip(chunks, vectors):
            chunk["vector"] = vec

        count = self.endee.upsert_chunks(chunks)
        return {
            "status": "success",
            "doc_name": doc_name,
            "chunks_indexed": count
        }

    # ─── SEMANTIC SEARCH ────────────────────────────────────────────────

    def semantic_search(self, query: str, top_k: int = 5, doc_name_filter: str = None) -> list[dict]:
        """
        Embed query → search Endee → return ranked results.
        Each result: {id, score, text, doc_name, chunk_index}
        """
        query_vec = self.embedder.embed(query)
        return self.endee.search(query_vec, top_k=top_k, doc_name_filter=doc_name_filter)

    # ─── RAG QUERY ──────────────────────────────────────────────────────

    def rag_query(self, question: str, top_k: int = 5) -> dict:
        """
        Full RAG: semantic search → build context → LLM → answer with sources.
        Returns: {question, answer, sources}
        """
        sources = self.semantic_search(question, top_k=top_k)

        if not sources:
            return {
                "question": question,
                "answer": "No relevant documents found. Please upload academic content first.",
                "sources": []
            }

        context = self._build_context(sources)
        prompt = self._build_prompt(question, context)
        answer = self._call_llm(prompt)

        return {
            "question": question,
            "answer": answer,
            "sources": sources
        }

    def _build_context(self, sources: list[dict]) -> str:
        parts = []
        for i, s in enumerate(sources, 1):
            parts.append(f"[{i}] (Source: {s['doc_name']})\n{s['text']}")
        return "\n\n".join(parts)

    def _build_prompt(self, question: str, context: str) -> str:
        return f"""You are an expert academic research assistant.
Answer the question using ONLY the provided context below.
If the answer cannot be found in the context, respond with:
"The uploaded documents do not contain sufficient information to answer this question."
Be concise, accurate, and cite which source number supports your answer.

CONTEXT:
{context}

QUESTION: {question}

ANSWER:"""

    def _call_llm(self, prompt: str) -> str:
        """
        Try OpenAI GPT-4o-mini first.
        Fallback to local google/flan-t5-base if no API key set.
        """
        if settings.openai_api_key:
            return self._openai_call(prompt)
        else:
            return self._local_llm_call(prompt)

    def _openai_call(self, prompt: str) -> str:
        from openai import OpenAI
        client = OpenAI(api_key=settings.openai_api_key)
        response = client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[{"role": "user", "content": prompt}],
            max_tokens=600,
            temperature=0.2
        )
        return response.choices[0].message.content.strip()

    def _local_llm_call(self, prompt: str) -> str:
        """Zero-cost local fallback using Flan-T5-base."""
        from transformers import pipeline
        # Use a short version of the prompt for T5 (it has token limits)
        short_prompt = prompt[:1500]
        pipe = pipeline("text2text-generation", model="google/flan-t5-base", max_new_tokens=300)
        result = pipe(short_prompt)
        return result[0]["generated_text"].strip()
