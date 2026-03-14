"""
collectors/config_file_scanner.py
──────────────────────────────────
Scans the file system for developer configuration files, shell scripts,
project manifests, and log files. Returns Document dicts suitable for
chunking and embedding.

Supported file types (configurable via CONFIG_SCAN_EXTENSIONS):
  .conf .cfg .yml .yaml .toml .ini .env .sh .ps1 .md .txt .log
  package.json  Makefile  Dockerfile  docker-compose.yml  nginx.conf  etc.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import chardet

from config import (
    CONFIG_SCAN_PATHS,
    CONFIG_SCAN_EXTENSIONS,
    MAX_SCAN_DEPTH,
)

Document = dict[str, Any]

# ── Hard limits ────────────────────────────────────────────────────────────────
MAX_FILE_SIZE_BYTES = 500_000   # 500 KB – skip giant log files
SKIP_DIRS = {
    ".git", ".svn", "node_modules", "__pycache__", ".venv", "venv",
    ".tox", "dist", "build", ".idea", ".vscode", ".mypy_cache",
    ".pytest_cache", "vendor",
}


# ── Helpers ────────────────────────────────────────────────────────────────────

def _safe_read(path: Path) -> str | None:
    try:
        if path.stat().st_size > MAX_FILE_SIZE_BYTES:
            return None
        raw = path.read_bytes()
        encoding = chardet.detect(raw).get("encoding") or "utf-8"
        return raw.decode(encoding, errors="replace")
    except (OSError, PermissionError):
        return None


def _is_target_file(path: Path, extensions: set[str]) -> bool:
    """Return True if the file matches by extension OR exact name."""
    exact_names = {
        "Makefile", "Dockerfile", "Procfile",
        "nginx.conf", "httpd.conf",
        "package.json", "pyproject.toml",
        "docker-compose.yml", "docker-compose.yaml",
        ".bashrc", ".zshrc", ".profile", ".bash_profile",
        ".gitconfig", ".gitignore", ".npmrc", ".yarnrc",
    }
    return path.suffix.lower() in extensions or path.name in exact_names


def _scan_path(
    root: Path,
    extensions: set[str],
    max_depth: int,
) -> list[tuple[Path, str]]:
    """Recursively scan *root* up to *max_depth* levels, returning (path, content) pairs."""
    results: list[tuple[Path, str]] = []

    if not root.exists():
        return results

    if root.is_file():
        content = _safe_read(root)
        if content is not None:
            results.append((root, content))
        return results

    for dirpath, dirnames, filenames in os.walk(root):
        current = Path(dirpath)
        depth   = len(current.relative_to(root).parts)

        if depth > max_depth:
            dirnames.clear()
            continue

        # Prune skip directories in-place so os.walk skips them
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]

        for fname in filenames:
            fpath = current / fname
            if _is_target_file(fpath, extensions):
                content = _safe_read(fpath)
                if content is not None:
                    results.append((fpath, content))

    return results


# ── Main collector ─────────────────────────────────────────────────────────────

def collect_config_files(
    scan_paths: list[str] | None = None,
    extensions: list[str] | None = None,
    max_depth: int | None = None,
) -> list[Document]:
    """
    Scan the configured paths for config/script files.

    Parameters
    ----------
    scan_paths:
        Override the default list of paths to scan.
    extensions:
        Override the default file extensions filter.
    max_depth:
        Override the maximum directory recursion depth.

    Returns
    -------
    list[Document]
        Each document represents one file's content.
    """
    paths_to_scan = scan_paths or CONFIG_SCAN_PATHS
    ext_set       = set(extensions or CONFIG_SCAN_EXTENSIONS)
    depth         = max_depth if max_depth is not None else MAX_SCAN_DEPTH

    docs: list[Document] = []
    seen: set[str]       = set()

    for raw_path in paths_to_scan:
        expanded = Path(raw_path).expanduser().resolve()
        for fpath, content in _scan_path(expanded, ext_set, depth):
            key = str(fpath)
            if key in seen or not content.strip():
                continue
            seen.add(key)

            # Infer a human-readable document type
            suffix = fpath.suffix.lower()
            if suffix in {".log", ".txt"}:
                doc_type = "log"
            elif fpath.name in {"Makefile", "Dockerfile", "Procfile"}:
                doc_type = "project_file"
            elif suffix in {".sh", ".ps1", ".bash"}:
                doc_type = "script"
            elif suffix in {".yml", ".yaml", ".toml", ".ini"}:
                doc_type = "config"
            elif fpath.name.startswith("."):
                doc_type = "dotfile"
            else:
                doc_type = "config"

            docs.append({
                "content":  content,
                "source":   str(fpath),
                "type":     doc_type,
                "metadata": {
                    "file":      str(fpath),
                    "filename":  fpath.name,
                    "extension": fpath.suffix,
                    "size":      len(content),
                },
            })

    print(f"[config_file_scanner] ✓  Found {len(docs)} config / script files")
    return docs


def collect_logs(log_dirs: list[str] | None = None) -> list[Document]:
    """
    Convenience function to collect only log files from common directories.
    """
    default_log_dirs = [
        "/var/log",
        "~/.local/share",
        "~/logs",
        "~/log",
    ]
    dirs = log_dirs or default_log_dirs
    return collect_config_files(
        scan_paths=dirs,
        extensions=[".log", ".txt"],
        max_depth=2,
    )
