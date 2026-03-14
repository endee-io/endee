"""
Search and Query Engine

Handles natural language queries over the indexed codebase.
Combines vector search with optional LLM-powered explanations.
"""

from typing import Optional
from dataclasses import dataclass
from openai import OpenAI
import requests
import re

from config import config
from endee_client import EndeeClient
from embeddings import EmbeddingGenerator


@dataclass
class SearchResult:
    """Represents a single search result."""
    id: str
    score: float
    file_path: str
    language: str
    start_line: int
    end_line: int
    chunk_type: str
    name: str
    content: str = ""  # Retrieved separately if needed
    
    def location(self) -> str:
        """Return a human-readable location string."""
        return f"{self.file_path}:{self.start_line}-{self.end_line}"


class SearchEngine:
    """
    Search engine for querying the indexed codebase.
    
    Supports:
    - Semantic search (natural language)
    - Filtered search (by file path, language, etc.)
    - RAG-style question answering
    """
    
    def __init__(
        self,
        index_name: Optional[str] = None,
        endee_client: Optional[EndeeClient] = None,
        embedding_generator: Optional[EmbeddingGenerator] = None,
    ):
        """
        Initialize the search engine.
        
        Args:
            index_name: Endee index name
            endee_client: Custom client
            embedding_generator: Custom embedder
        """
        self.index_name = index_name or config.INDEX_NAME
        self.client = endee_client or EndeeClient()
        self.embedder = embedding_generator or EmbeddingGenerator()

        self.llm_provider = config.LLM_PROVIDER.strip().lower()
        self.ollama_base_url = config.OLLAMA_BASE_URL.rstrip("/")
        self.ollama_model = config.OLLAMA_MODEL

        # LLM client for RAG (OpenAI only)
        self.llm_client = (
            OpenAI(
                api_key=(config.OPENAI_API_KEY or "lm-studio"),
                base_url=(config.OPENAI_BASE_URL or None),
            )
            if self.llm_provider == "openai" and (config.OPENAI_API_KEY or config.OPENAI_BASE_URL)
            else None
        )
    
    def search(
        self,
        query: str,
        top_k: int = 10,
        language: Optional[str] = None,
        file_pattern: Optional[str] = None,
        chunk_type: Optional[str] = None,
    ) -> list[SearchResult]:
        """
        Search the codebase with natural language.
        
        Args:
            query: Natural language query (e.g., "authentication logic")
            top_k: Number of results to return
            language: Filter by programming language
            file_pattern: Filter by file path pattern
            chunk_type: Filter by chunk type (function, class, etc.)
        
        Returns:
            List of SearchResult objects
        """
        # Generate query embedding
        query_vector = self.embedder.embed(query)
        
        # Build filter conditions
        filters = self._build_filters(language, file_pattern, chunk_type)
        
        # Search Endee
        raw_results = self.client.search(
            index_name=self.index_name,
            query_vector=query_vector,
            top_k=top_k,
            filter_conditions=filters,
        )
        
        # Convert to SearchResult objects
        results = []
        for r in raw_results:
            meta = r.get("metadata", {})
            results.append(SearchResult(
                id=r.get("id", ""),
                score=r.get("score", 0.0),
                file_path=meta.get("file_path", ""),
                language=meta.get("language", ""),
                start_line=meta.get("start_line", 0),
                end_line=meta.get("end_line", 0),
                chunk_type=meta.get("chunk_type", ""),
                name=meta.get("name", ""),
            ))
        
        return results
    
    def search_similar(
        self,
        code_snippet: str,
        top_k: int = 10,
        exclude_file: Optional[str] = None,
    ) -> list[SearchResult]:
        """
        Find code similar to a given snippet.
        
        Args:
            code_snippet: Code to find similar matches for
            top_k: Number of results
            exclude_file: Optionally exclude results from this file
        
        Returns:
            List of similar code chunks
        """
        # Embed the snippet
        query_vector = self.embedder.embed(code_snippet)
        
        # Build filter to exclude source file if specified
        filters = None
        if exclude_file:
            filters = {
                "file_path": {"$ne": exclude_file}
            }
        
        raw_results = self.client.search(
            index_name=self.index_name,
            query_vector=query_vector,
            top_k=top_k,
            filter_conditions=filters,
        )
        
        return [
            SearchResult(
                id=r.get("id", ""),
                score=r.get("score", 0.0),
                file_path=r.get("metadata", {}).get("file_path", ""),
                language=r.get("metadata", {}).get("language", ""),
                start_line=r.get("metadata", {}).get("start_line", 0),
                end_line=r.get("metadata", {}).get("end_line", 0),
                chunk_type=r.get("metadata", {}).get("chunk_type", ""),
                name=r.get("metadata", {}).get("name", ""),
            )
            for r in raw_results
        ]
    
    def ask(
        self,
        question: str,
        top_k: int = 5,
        language: Optional[str] = None,
        file_pattern: Optional[str] = None,
    ) -> dict:
        """
        Ask a question about the codebase using RAG.
        
        This retrieves relevant code chunks and uses an LLM to generate
        an answer based on the context.
        
        Args:
            question: Natural language question
            top_k: Number of context chunks to retrieve
            language: Optional language filter
            file_pattern: Optional file pattern filter
        
        Returns:
            Dict with 'answer', 'sources', and 'context'
        """
        # Step 1: Retrieve a wider candidate set
        candidate_k = max(top_k * 4, 20)
        candidates = self.search(
            query=question,
            top_k=candidate_k,
            language=language,
            file_pattern=file_pattern,
        )

        # Optional module-aware fallback if query hints at a specific subsystem
        module_hint = self._detect_module_hint(question)
        if module_hint and not any(module_hint in (r.file_path or "").lower() for r in candidates):
            hinted = self.search(
                query=question,
                top_k=candidate_k,
                language=language,
                file_pattern=f"src/{module_hint}",
            )
            seen = {r.id for r in candidates}
            for item in hinted:
                if item.id not in seen:
                    candidates.append(item)
                    seen.add(item.id)

        # Rerank by keyword overlap with metadata fields and keep final top_k
        results = self._rerank_results(question, candidates, top_k)
        
        if not results:
            return {
                "answer": "I couldn't find any relevant code for your question.",
                "sources": [],
                "context": "",
            }
        
        # Step 2: Fetch actual code content for context
        context_parts = []
        for result in results:
            code_content = self._fetch_code_content(result)
            if code_content:
                context_parts.append(
                    f"--- {result.location()} ({result.language}) ---\n{code_content}"
                )
        
        context = "\n\n".join(context_parts)
        
        # Step 3: Generate answer with configured LLM provider; fallback on failure
        try:
            answer = self._generate_answer(question, context)
        except Exception as exc:
            answer = self._generate_fallback_answer(question, results, str(exc))
        
        return {
            "answer": answer,
            "sources": [r.location() for r in results],
            "context": context,
        }

    def _rerank_results(
        self,
        question: str,
        candidates: list[SearchResult],
        top_k: int,
    ) -> list[SearchResult]:
        """Rerank retrieved candidates using keyword overlap with metadata fields."""
        keywords = self._extract_keywords(question)

        if not candidates:
            return []

        scored: list[tuple[float, SearchResult]] = []
        for item in candidates:
            haystack = " ".join(
                [
                    (item.file_path or "").lower(),
                    (item.name or "").lower(),
                    (item.chunk_type or "").lower(),
                    (item.language or "").lower(),
                ]
            )

            overlap = sum(1 for kw in keywords if kw in haystack)
            bonus = min(0.6, overlap * 0.12)
            score = float(item.score) + bonus
            scored.append((score, item))

        scored.sort(key=lambda pair: pair[0], reverse=True)
        return [item for _, item in scored[:top_k]]

    def _extract_keywords(self, text: str) -> list[str]:
        """Extract significant query tokens for reranking."""
        stop_words = {
            "how", "does", "work", "works", "what", "where", "when", "why", "the", "and",
            "for", "with", "this", "that", "codebase", "about", "from", "into", "which",
            "are", "is", "in", "to", "of", "a", "an",
        }
        tokens = re.findall(r"[a-zA-Z_][a-zA-Z0-9_\-]{2,}", text.lower())
        return [t for t in tokens if t not in stop_words]

    def _detect_module_hint(self, question: str) -> Optional[str]:
        """Infer likely subsystem folder from question text."""
        q = question.lower()
        modules = ["hnsw", "sparse", "filter", "storage", "server", "quant", "core", "utils"]
        for module in modules:
            if module in q:
                return module
        return None

    def _generate_fallback_answer(
        self,
        question: str,
        results: list[SearchResult],
        error_message: Optional[str] = None,
    ) -> str:
        """Generate an extractive fallback answer when LLM is unavailable."""
        lines = []
        if error_message:
            lines.append(
                "LLM response is unavailable right now (likely quota/billing/API issue), "
                "so this is a retrieval-only answer."
            )
        else:
            lines.append("LLM is not configured, so this is a retrieval-only answer.")

        lines.append(f"Question: {question}")
        lines.append("Most relevant code locations:")

        for i, result in enumerate(results[:5], 1):
            label = result.name or "unnamed"
            lines.append(
                f"{i}. {result.location()} | {result.language} | {result.chunk_type} | {label}"
            )

        lines.append(
            "Use the listed sources to inspect exact implementation details in the codebase."
        )
        return "\n".join(lines)
    
    def _build_filters(
        self,
        language: Optional[str] = None,
        file_pattern: Optional[str] = None,
        chunk_type: Optional[str] = None,
    ) -> Optional[dict]:
        """Build Endee filter conditions from parameters."""
        conditions = {}
        
        if language:
            conditions["language"] = language
        
        if file_pattern:
            conditions["file_path"] = file_pattern
        
        if chunk_type:
            conditions["chunk_type"] = chunk_type
        
        return conditions if conditions else None
    
    def _fetch_code_content(self, result: SearchResult) -> str:
        """
        Fetch the actual code content for a search result.
        
        This reads the file directly since Endee stores vectors, not content.
        """
        try:
            with open(result.file_path, "r", encoding="utf-8", errors="ignore") as f:
                lines = f.readlines()
                
            start = max(0, result.start_line - 1)
            end = min(len(lines), result.end_line)
            
            return "".join(lines[start:end])
        except Exception:
            return ""
    
    def _generate_answer(self, question: str, context: str) -> str:
        """Generate an answer using the LLM."""
        if self.llm_provider == "ollama":
            return self._generate_answer_ollama(question, context)

        llm_client = self.llm_client
        if llm_client is None:
            raise ValueError("LLM client not available. Set LLM_PROVIDER and related configuration.")
        
        system_prompt = """You are an expert code analyst. Answer questions about the codebase based on the provided code context.

Guidelines:
- Be concise and direct
- Reference specific files and line numbers when relevant
- If the context doesn't contain enough information, say so
- Format code snippets with appropriate markdown"""

        user_prompt = f"""Based on the following code from the codebase, answer this question:

Question: {question}

Code Context:
{context}

Answer:"""

        response = llm_client.chat.completions.create(
            model=config.OPENAI_MODEL,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
            temperature=0.3,
            max_tokens=1000,
        )
        
        return response.choices[0].message.content or ""

    def _generate_answer_ollama(self, question: str, context: str) -> str:
        """Generate an answer with local Ollama."""
        system_prompt = (
            "You are an expert code analyst. Answer using only the provided code context. "
            "Be concise and reference files/lines when possible."
        )
        user_prompt = (
            f"Question: {question}\n\n"
            f"Code Context:\n{context}\n\n"
            "Answer:"
        )

        payload = {
            "model": self.ollama_model,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
            "stream": False,
        }
        response = requests.post(
            f"{self.ollama_base_url}/api/chat",
            json=payload,
            timeout=120,
        )
        response.raise_for_status()
        data = response.json()
        message = data.get("message", {})
        return message.get("content", "")
    
    def explain(self, file_path: str, start_line: int, end_line: int) -> str:
        """
        Explain a specific section of code.
        
        Args:
            file_path: Path to the file
            start_line: Starting line number
            end_line: Ending line number
        
        Returns:
            LLM-generated explanation
        """
        # Read the code
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                lines = f.readlines()
            code = "".join(lines[start_line - 1:end_line])
        except Exception as e:
            return f"Error reading file: {e}"
        
        prompt = f"""Explain this code clearly and concisely:

File: {file_path}
Lines: {start_line}-{end_line}

```
{code}
```

Provide:
1. A brief summary of what this code does
2. Key implementation details
3. Any notable patterns or practices used"""

        if self.llm_provider == "ollama":
            payload = {
                "model": self.ollama_model,
                "messages": [
                    {"role": "user", "content": prompt},
                ],
                "stream": False,
            }
            response = requests.post(
                f"{self.ollama_base_url}/api/chat",
                json=payload,
                timeout=120,
            )
            response.raise_for_status()
            data = response.json()
            return data.get("message", {}).get("content", "")

        llm_client = self.llm_client
        if llm_client is None:
            raise ValueError("LLM client not available. Set LLM_PROVIDER and related configuration.")

        response = llm_client.chat.completions.create(
            model=config.OPENAI_MODEL,
            messages=[{"role": "user", "content": prompt}],
            temperature=0.3,
            max_tokens=800,
        )
        
        return response.choices[0].message.content or ""


# Convenience function
def create_search_engine(index_name: Optional[str] = None) -> SearchEngine:
    """Create a configured search engine instance."""
    return SearchEngine(index_name=index_name)
