"""
Central configuration for the RAG pipeline.
Loads settings from environment variables with sensible defaults.
"""

import os
from dataclasses import dataclass, field
from dotenv import load_dotenv

# Load .env file if present
load_dotenv()


@dataclass
class Settings:
    """All configurable settings for the RAG pipeline."""

    # ── Endee Vector Database ──────────────────────────────────────────
    endee_url: str = field(
        default_factory=lambda: os.getenv("ENDEE_URL", "http://localhost:8080")
    )
    endee_auth_token: str = field(
        default_factory=lambda: os.getenv("ENDEE_AUTH_TOKEN", "")
    )
    endee_index_name: str = field(
        default_factory=lambda: os.getenv("ENDEE_INDEX_NAME", "rag-documents")
    )

    # ── Embedding Model ────────────────────────────────────────────────
    embedding_model: str = field(
        default_factory=lambda: os.getenv(
            "EMBEDDING_MODEL", "sentence-transformers/all-MiniLM-L6-v2"
        )
    )
    embedding_dimension: int = field(
        default_factory=lambda: int(os.getenv("EMBEDDING_DIMENSION", "384"))
    )

    # ── Chunking ───────────────────────────────────────────────────────
    chunk_size: int = field(
        default_factory=lambda: int(os.getenv("CHUNK_SIZE", "500"))
    )
    chunk_overlap: int = field(
        default_factory=lambda: int(os.getenv("CHUNK_OVERLAP", "50"))
    )

    # ── Retrieval ──────────────────────────────────────────────────────
    top_k: int = field(
        default_factory=lambda: int(os.getenv("TOP_K", "5"))
    )

    # ── LLM Provider ──────────────────────────────────────────────────
    # Supported: "gemini", "openai", "claude"
    llm_provider: str = field(
        default_factory=lambda: os.getenv("LLM_PROVIDER", "gemini")
    )
    llm_model: str = field(
        default_factory=lambda: os.getenv("LLM_MODEL", "")
    )

    # ── API Keys ──────────────────────────────────────────────────────
    gemini_api_key: str = field(
        default_factory=lambda: os.getenv("GEMINI_API_KEY", "")
    )
    openai_api_key: str = field(
        default_factory=lambda: os.getenv("OPENAI_API_KEY", "")
    )
    anthropic_api_key: str = field(
        default_factory=lambda: os.getenv("ANTHROPIC_API_KEY", "")
    )

    # ── API Server ────────────────────────────────────────────────────
    api_host: str = field(
        default_factory=lambda: os.getenv("API_HOST", "0.0.0.0")
    )
    api_port: int = field(
        default_factory=lambda: int(os.getenv("API_PORT", "8000"))
    )

    @property
    def active_llm_model(self) -> str:
        """Return the model name, using a sensible default per provider."""
        if self.llm_model:
            return self.llm_model
        defaults = {
            "gemini": "gemini-2.0-flash",
            "openai": "gpt-4o-mini",
            "claude": "claude-sonnet-4-20250514",
        }
        return defaults.get(self.llm_provider, "gemini-2.0-flash")

    @property
    def endee_headers(self) -> dict:
        """Build HTTP headers for Endee API requests."""
        headers = {"Content-Type": "application/json"}
        if self.endee_auth_token:
            headers["Authorization"] = self.endee_auth_token
        return headers


# Singleton instance used throughout the project
settings = Settings()
