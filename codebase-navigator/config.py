"""
Configuration management for the Codebase Navigator.
Loads settings from environment variables.
"""

import os
from dotenv import load_dotenv

# Load environment variables from .env file
load_dotenv()


class Config:
    """Application configuration loaded from environment variables."""
    
    # Endee Database
    ENDEE_URL: str = os.getenv("ENDEE_URL", "http://localhost:8080")
    ENDEE_API_KEY: str = os.getenv("ENDEE_API_KEY", "")
    
    # OpenAI
    OPENAI_API_KEY: str = os.getenv("OPENAI_API_KEY", "")
    OPENAI_BASE_URL: str = os.getenv("OPENAI_BASE_URL", "")
    OPENAI_MODEL: str = os.getenv("OPENAI_MODEL", "gpt-4o-mini")

    # LLM provider settings
    LLM_PROVIDER: str = os.getenv("LLM_PROVIDER", "ollama")  # ollama | openai | none
    OLLAMA_BASE_URL: str = os.getenv("OLLAMA_BASE_URL", "http://127.0.0.1:11434")
    OLLAMA_MODEL: str = os.getenv("OLLAMA_MODEL", "llama3.2:3b")
    
    # Embedding settings
    EMBEDDING_MODEL: str = os.getenv("EMBEDDING_MODEL", "text-embedding-3-small")
    EMBEDDING_DIMENSIONS: int = int(os.getenv("EMBEDDING_DIMENSIONS", "1536"))
    
    # Index settings
    INDEX_NAME: str = os.getenv("INDEX_NAME", "codebase")
    
    # Chunking settings
    MAX_CHUNK_SIZE: int = 1500  # Max characters per chunk
    CHUNK_OVERLAP: int = 200   # Overlap between chunks
    
    # Supported file extensions
    SUPPORTED_EXTENSIONS: dict = {
        ".py": "python",
        ".js": "javascript",
        ".ts": "typescript",
        ".tsx": "typescript",
        ".jsx": "javascript",
        ".cpp": "cpp",
        ".hpp": "cpp",
        ".c": "c",
        ".h": "c",
        ".java": "java",
        ".go": "go",
        ".rs": "rust",
        ".rb": "ruby",
        ".php": "php",
        ".cs": "csharp",
        ".md": "markdown",
        ".txt": "text",
    }
    
    @classmethod
    def validate(cls, require_llm: bool = False) -> list[str]:
        """Validate required configuration. Returns list of missing items."""
        missing = []
        if require_llm:
            provider = cls.LLM_PROVIDER.strip().lower()
            if provider == "openai" and not cls.OPENAI_API_KEY and not cls.OPENAI_BASE_URL:
                missing.append("OPENAI_API_KEY")
            if provider == "ollama" and not cls.OLLAMA_MODEL:
                missing.append("OLLAMA_MODEL")
        return missing


# Global config instance
config = Config()
