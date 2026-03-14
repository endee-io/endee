"""
collectors/powershell_history_collector.py
──────────────────────────────────────────
Reads the Windows PowerShell (PSReadLine) console history file that is
accessible from inside WSL via the /mnt/c/ path.

The file is typically located at:
  %APPDATA%\\Microsoft\\Windows\\PowerShell\\PSReadLine\\ConsoleHost_history.txt

Inside WSL this maps to:
  /mnt/c/Users/<USERNAME>/AppData/Roaming/Microsoft/Windows/PowerShell/
           PSReadLine/ConsoleHost_history.txt
"""

from __future__ import annotations

import getpass
import re
from pathlib import Path
from typing import Any

import chardet

from config import PS_HISTORY_PATH

Document = dict[str, Any]

# ── WSL-aware path resolution ──────────────────────────────────────────────────

def _resolve_ps_history_path() -> Path:
    """
    Resolve the PowerShell history path in order of preference:
      1. The path from config / .env  (PS_HISTORY_PATH)
      2. WSL /mnt/c path for the current Windows username
      3. Standard Windows %APPDATA% path (works on native Windows Python)
    """
    candidates: list[Path] = [PS_HISTORY_PATH]

    # Try to derive the Windows username from /mnt/c/Users/
    wsl_users = Path("/mnt/c/Users")
    if wsl_users.exists():
        # Prefer the directory whose name matches the current Linux user
        linux_user = getpass.getuser()
        for user_dir in wsl_users.iterdir():
            if user_dir.is_dir() and not user_dir.name.startswith("."):
                ps_path = (
                    user_dir
                    / "AppData/Roaming/Microsoft/Windows/PowerShell"
                    / "PSReadLine/ConsoleHost_history.txt"
                )
                if user_dir.name.lower() == linux_user.lower():
                    candidates.insert(0, ps_path)   # highest priority
                else:
                    candidates.append(ps_path)

    for p in candidates:
        if p.exists():
            return p

    return PS_HISTORY_PATH   # fallback – caller handles missing file


# ── Helpers ────────────────────────────────────────────────────────────────────

def _safe_read(path: Path) -> str:
    try:
        raw = path.read_bytes()
        encoding = chardet.detect(raw).get("encoding") or "utf-16"
        return raw.decode(encoding, errors="replace")
    except (FileNotFoundError, PermissionError, OSError):
        return ""


def _clean_ps_line(line: str) -> str:
    """Remove PSReadLine internal markers."""
    return re.sub(r"^\s*#.*", "", line).strip()


def _group_commands(lines: list[str], group_size: int = 15) -> list[str]:
    groups: list[str] = []
    for i in range(0, len(lines), group_size):
        block = "\n".join(lines[i : i + group_size])
        if block.strip():
            groups.append(block)
    return groups


# ── Main collector ─────────────────────────────────────────────────────────────

def collect_powershell_history(
    path: Path | None = None,
    group_size: int = 15,
) -> list[Document]:
    """
    Collect PowerShell commands from the PSReadLine history file.

    Parameters
    ----------
    path:
        Override auto-detected path.
    group_size:
        Commands per Document block.

    Returns
    -------
    list[Document]
    """
    target = Path(path) if path else _resolve_ps_history_path()
    raw    = _safe_read(target)

    if not raw:
        print(f"[ps_history_collector] ⚠  Could not read {target} – skipping.")
        return []

    lines  = [_clean_ps_line(l) for l in raw.splitlines()]
    lines  = [l for l in lines if l]
    groups = _group_commands(lines, group_size)
    docs: list[Document] = []

    for idx, block in enumerate(groups):
        docs.append({
            "content": block,
            "source":  str(target),
            "type":    "powershell_history",
            "metadata": {
                "file":        str(target),
                "block_index": idx,
                "total_cmds":  len(lines),
            },
        })

    print(
        f"[ps_history_collector] ✓  {len(lines)} commands → "
        f"{len(docs)} documents from {target}"
    )
    return docs
