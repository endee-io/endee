# WSL Sensei – configuration and shared settings

import os
from pathlib import Path
from dotenv import load_dotenv

load_dotenv()

# ── Endee ─────────────────────────────────────────────────────────────────────
ENDEE_BASE_URL   = os.getenv("ENDEE_BASE_URL", "http://localhost:8080/api/v1")
ENDEE_AUTH_TOKEN = os.getenv("ENDEE_AUTH_TOKEN", "")          # empty = no auth
ENDEE_INDEX_NAME = os.getenv("ENDEE_INDEX_NAME", "wsl_sensei")

# ── Embedding model ───────────────────────────────────────────────────────────
EMBEDDING_MODEL     = os.getenv("EMBEDDING_MODEL", "all-MiniLM-L6-v2")
EMBEDDING_DIMENSION = int(os.getenv("EMBEDDING_DIMENSION", "384"))

# ── LLM backend ───────────────────────────────────────────────────────────────
# Options: "openai" | "ollama" | "anthropic" | "mock"
LLM_BACKEND       = os.getenv("LLM_BACKEND", "ollama")
OPENAI_API_KEY    = os.getenv("OPENAI_API_KEY", "")
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")

# Ollama (local)
OLLAMA_BASE_URL = os.getenv("OLLAMA_BASE_URL", "http://localhost:11434/v1")
OLLAMA_MODEL    = os.getenv("OLLAMA_MODEL", "llama3.2")

# OpenAI
OPENAI_MODEL = os.getenv("OPENAI_MODEL", "gpt-4o-mini")

# Anthropic
ANTHROPIC_MODEL = os.getenv("ANTHROPIC_MODEL", "claude-3-haiku-20240307")

# ── Chunking ──────────────────────────────────────────────────────────────────
CHUNK_SIZE    = int(os.getenv("CHUNK_SIZE", "300"))    # characters
CHUNK_OVERLAP = int(os.getenv("CHUNK_OVERLAP", "50"))

# ── RAG ───────────────────────────────────────────────────────────────────────
RAG_TOP_K      = int(os.getenv("RAG_TOP_K", "6"))
RAG_MAX_TOKENS = int(os.getenv("RAG_MAX_TOKENS", "1024"))

# ── Paths ─────────────────────────────────────────────────────────────────────
PROJECT_ROOT = Path(__file__).parent
DATA_DIR     = PROJECT_ROOT / "data"
SAMPLE_DIR   = DATA_DIR / "sample"

# Collector targets
BASH_HISTORY_PATH = Path(os.getenv("BASH_HISTORY_PATH", "~/.bash_history")).expanduser()
PS_HISTORY_PATH   = Path(os.getenv(
    "PS_HISTORY_PATH",
    r"~/AppData/Roaming/Microsoft/Windows/PowerShell/PSReadLine/ConsoleHost_history.txt"
)).expanduser()

CONFIG_SCAN_PATHS = [
    p.strip()
    for p in os.getenv(
        "CONFIG_SCAN_PATHS",
        "~/.bashrc,~/.zshrc,~/.profile,~/.gitconfig,~/.ssh/config,~/nginx.conf"
    ).split(",")
    if p.strip()
]
CONFIG_SCAN_EXTENSIONS = [
    e.strip()
    for e in os.getenv(
        "CONFIG_SCAN_EXTENSIONS",
        ".conf,.cfg,.yml,.yaml,.toml,.ini,.env,.sh,.ps1,.md"
    ).split(",")
    if e.strip()
]
MAX_SCAN_DEPTH = int(os.getenv("MAX_SCAN_DEPTH", "3"))
