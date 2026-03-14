"""
AI Codebase Navigator

A tool for semantic code search and Q&A using Endee vector database.
"""

__version__ = "0.1.0"
__author__ = "Your Name"

from .config import config
from .parser import CodeParser, CodeChunk, parse_codebase
from .embeddings import EmbeddingGenerator, SparseVectorGenerator
from .endee_client import EndeeClient, EndeeError
from .indexer import CodebaseIndexer, index_codebase
from .search import SearchEngine, SearchResult

__all__ = [
    "config",
    "CodeParser",
    "CodeChunk", 
    "parse_codebase",
    "EmbeddingGenerator",
    "SparseVectorGenerator",
    "EndeeClient",
    "EndeeError",
    "CodebaseIndexer",
    "index_codebase",
    "SearchEngine",
    "SearchResult",
]
