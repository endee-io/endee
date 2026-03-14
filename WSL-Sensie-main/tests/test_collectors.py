"""Unit tests for collectors (uses sample data, no live environment needed)."""

import tempfile
from pathlib import Path

import pytest

from collectors.bash_history_collector    import collect_bash_history
from collectors.config_file_scanner       import collect_config_files


# ── Bash history collector ────────────────────────────────────────────────────

def test_bash_history_real_file():
    """Create a temp history file and verify collection."""
    commands = ["git status", "npm install", "docker ps", "ls -la"]
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
        f.write("\n".join(commands))
        tmp = Path(f.name)

    docs = collect_bash_history(path=tmp, group_size=2)
    tmp.unlink()

    assert len(docs) > 0
    for doc in docs:
        assert doc["type"]    == "bash_history"
        assert doc["content"]
        assert doc["source"]  == str(tmp)


def test_bash_history_missing_file():
    """Missing file should return empty list, not raise."""
    docs = collect_bash_history(path=Path("/nonexistent/history.txt"))
    assert docs == []


def test_bash_history_group_size():
    """Each group should contain at most `group_size` commands."""
    commands = [f"cmd_{i}" for i in range(100)]
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
        f.write("\n".join(commands))
        tmp = Path(f.name)

    docs = collect_bash_history(path=tmp, group_size=10)
    tmp.unlink()

    for doc in docs:
        lines = [l for l in doc["content"].splitlines() if l.strip()]
        assert len(lines) <= 10


# ── Config file scanner ───────────────────────────────────────────────────────

def test_config_scanner_sample_dir():
    """Should find files in the bundled sample data directory."""
    sample_dir = Path(__file__).parent.parent / "data" / "sample"
    docs = collect_config_files(
        scan_paths=[str(sample_dir)],
        extensions=[".sh", ".conf", ".yml", ".toml", ".txt", ".log"],
        max_depth=1,
    )
    assert len(docs) > 0
    for doc in docs:
        assert doc["content"]
        assert doc["source"]
        assert doc["type"]


def test_config_scanner_empty_dir():
    """Empty directory should return no documents."""
    with tempfile.TemporaryDirectory() as tmp:
        docs = collect_config_files(scan_paths=[tmp], max_depth=1)
    assert docs == []


def test_config_scanner_specific_extension():
    """Should only return files matching the given extension."""
    sample_dir = Path(__file__).parent.parent / "data" / "sample"
    docs = collect_config_files(
        scan_paths=[str(sample_dir)],
        extensions=[".conf"],
        max_depth=1,
    )
    for doc in docs:
        assert doc["source"].endswith(".conf"), f"Unexpected extension: {doc['source']}"


def test_config_scanner_deduplication():
    """Same path scanned twice should not produce duplicates."""
    sample_dir = str(Path(__file__).parent.parent / "data" / "sample")
    docs1 = collect_config_files(scan_paths=[sample_dir], max_depth=1)
    docs2 = collect_config_files(scan_paths=[sample_dir, sample_dir], max_depth=1)
    assert len(docs1) == len(docs2)


def test_config_scanner_respects_max_depth():
    """Files beyond max_depth should not be returned."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # Create a deep file
        deep = root / "a" / "b" / "c" / "deep.conf"
        deep.parent.mkdir(parents=True)
        deep.write_text("[deep config]")

        # With max_depth=1 we should NOT find the file (depth=3)
        docs = collect_config_files(
            scan_paths=[str(root)],
            extensions=[".conf"],
            max_depth=1,
        )
        assert not any("deep.conf" in d["source"] for d in docs)

        # With max_depth=4 we should find it
        docs = collect_config_files(
            scan_paths=[str(root)],
            extensions=[".conf"],
            max_depth=4,
        )
        assert any("deep.conf" in d["source"] for d in docs)
