"""
collectors/bash_history_collector.py
─────────────────────────────────────
Reads the user's Bash (and optionally Zsh) shell history file and returns
a list of Document dicts ready for chunking and indexing.

Each Document carries:
  - content  : the raw text block
  - source   : file path
  - type     : "bash_history" | "zsh_history"
  - metadata : additional key/value pairs
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import chardet

from config import BASH_HISTORY_PATH

# ── Types ─────────────────────────────────────────────────────────────────────

Document = dict[str, Any]


# ── Helpers ───────────────────────────────────────────────────────────────────

def _safe_read(path: Path) -> str:
    """Read a file with automatic encoding detection; return empty string on error."""
    try:
        raw = path.read_bytes()
        encoding = chardet.detect(raw).get("encoding") or "utf-8"
        return raw.decode(encoding, errors="replace")
    except (FileNotFoundError, PermissionError, OSError):
        return ""


def _clean_bash_line(line: str) -> str:
    """Strip timestamp prefixes added by HISTTIMEFORMAT."""
    # e.g.  "#1711234567\ngit status"  →  "git status"
    return re.sub(r"^#\d+\s*", "", line).strip()


def _group_commands(lines: list[str], group_size: int = 20) -> list[str]:
    """
    Bundle consecutive commands into blocks so each Document captures
    a meaningful slice of work-flow context rather than a single command.
    """
    groups: list[str] = []
    for i in range(0, len(lines), group_size):
        block = "\n".join(lines[i : i + group_size])
        if block.strip():
            groups.append(block)
    return groups


# ── Main collector ────────────────────────────────────────────────────────────

def collect_bash_history(
    path: Path | None = None,
    group_size: int = 20,
) -> list[Document]:
    """
    Collect commands from ``~/.bash_history`` (or a custom path).

    Parameters
    ----------
    path:
        Override the default history file location.
    group_size:
        How many consecutive commands to bundle into one Document chunk.

    Returns
    -------
    list[Document]
        Each item is a dict with keys ``content``, ``source``, ``type``,
        and ``metadata``.
    """
    target = Path(path) if path else BASH_HISTORY_PATH
    raw    = _safe_read(target)

    if not raw:
        print(f"[bash_history_collector] ⚠  Could not read {target} – skipping.")
        return []

    lines = [_clean_bash_line(l) for l in raw.splitlines()]
    lines = [l for l in lines if l and not l.startswith("#")]

    groups   = _group_commands(lines, group_size)
    docs: list[Document] = []

    for idx, block in enumerate(groups):
        docs.append({
            "content": block,
            "source":  str(target),
            "type":    "bash_history",
            "metadata": {
                "file":        str(target),
                "block_index": idx,
                "total_cmds":  len(lines),
            },
        })

    print(f"[bash_history_collector] ✓  {len(lines)} commands → {len(docs)} documents from {target}")
    return docs


def collect_zsh_history(group_size: int = 20) -> list[Document]:
    """Convenience wrapper for Zsh's extended history format."""
    zsh_path = Path("~/.zsh_history").expanduser()
    docs = collect_bash_history(path=zsh_path, group_size=group_size)
    for doc in docs:
        doc["type"] = "zsh_history"
    return docs


def collect_all_shell_history(group_size: int = 20) -> list[Document]:
    """Collect from both Bash and Zsh; deduplicate by content."""
    seen:  set[str]     = set()
    all_docs: list[Document] = []

    for doc in collect_bash_history(group_size=group_size) + collect_zsh_history(group_size=group_size):
        key = doc["content"]
        if key not in seen:
            seen.add(key)
            all_docs.append(doc)

    return all_docs
