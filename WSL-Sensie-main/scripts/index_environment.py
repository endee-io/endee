"""
scripts/index_environment.py
─────────────────────────────
Standalone script to index the full developer environment into Endee.

Usage
─────
  # Index everything (default)
  python scripts/index_environment.py

  # Index specific sources
  python scripts/index_environment.py --sources bash_history config_files

  # Force a clean rebuild
  python scripts/index_environment.py --rebuild

  # Dry-run: collect + chunk but do NOT write to Endee
  python scripts/index_environment.py --dry-run

  # Use the bundled sample data (for demos / CI)
  python scripts/index_environment.py --sample
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# Ensure project root is on sys.path so local imports work
sys.path.insert(0, str(Path(__file__).parent.parent))

from rich.console import Console
from rich.table   import Table

console = Console()


def banner():
    console.print("""
[bold blue]
 __      _____ _      ___
 \\ \\    / / __| |    / __|___ _ _  ___ ___ ___
  \\ \\/\\/ /\\__ \\ |__ \\__ \\/ -_) ' \\(_-</ -_) |
   \\_/\\_/ |___/____||___/\\___|_||_/__/\\___|\\_|
[/]
[bold]WSL Sensei – Environment Indexer[/]
""")


def load_sample_data() -> list[dict]:
    """
    Load the bundled sample documents from data/sample/.
    Used for demos, testing, and CI environments where the real
    shell history / config files may not be present.
    """
    from collectors.config_file_scanner import collect_config_files

    sample_dir = Path(__file__).parent.parent / "data" / "sample"
    console.print(f"[cyan]Loading sample data from {sample_dir} …[/]")

    docs = collect_config_files(
        scan_paths=[str(sample_dir)],
        extensions=[".sh", ".conf", ".yaml", ".yml", ".toml", ".md",
                    ".env", ".txt", ".log", ".ps1"],
        max_depth=2,
    )
    return docs


def run_indexing(
    sources:       list[str],
    force_rebuild: bool = False,
    dry_run:       bool = False,
    use_sample:    bool = False,
) -> dict:
    from collectors.bash_history_collector       import collect_bash_history
    from collectors.powershell_history_collector import collect_powershell_history
    from collectors.config_file_scanner          import collect_config_files
    from indexing.chunker                        import chunk_documents
    from indexing.embed_store                    import store_chunks, delete_index, ensure_index

    t0 = time.time()

    # ── 1. Collect ────────────────────────────────────────────────────────────
    console.print("\n[bold cyan]Step 1 – Collecting documents …[/]")
    docs = []

    if use_sample:
        docs = load_sample_data()
    else:
        if "bash_history" in sources:
            console.print("  → bash history")
            docs += collect_bash_history()

        if "powershell_history" in sources:
            console.print("  → PowerShell history")
            docs += collect_powershell_history()

        if "config_files" in sources:
            console.print("  → config / script files")
            docs += collect_config_files()

    console.print(f"  [green]✓[/]  {len(docs)} documents collected")

    if not docs:
        console.print("[yellow]⚠  No documents found. Nothing to index.[/]")
        return {"docs": 0, "chunks": 0, "stored": 0, "duration": 0}

    # ── 2. Chunk ──────────────────────────────────────────────────────────────
    console.print("\n[bold cyan]Step 2 – Chunking documents …[/]")
    chunks = chunk_documents(docs)
    console.print(f"  [green]✓[/]  {len(chunks)} chunks created")

    if dry_run:
        console.print("\n[yellow]🔍 DRY-RUN mode – skipping Endee write.[/]")
        _print_chunk_preview(chunks[:10])
        return {"docs": len(docs), "chunks": len(chunks), "stored": 0,
                "duration": time.time() - t0, "dry_run": True}

    # ── 3. Rebuild / ensure index ─────────────────────────────────────────────
    console.print("\n[bold cyan]Step 3 – Preparing Endee index …[/]")
    if force_rebuild:
        console.print("  → Deleting existing index …")
        delete_index()
    ensure_index()
    console.print("  [green]✓[/]  Index ready")

    # ── 4. Embed + store ──────────────────────────────────────────────────────
    console.print("\n[bold cyan]Step 4 – Embedding + storing vectors in Endee …[/]")
    stored = store_chunks(chunks)
    console.print(f"  [green]✓[/]  {stored} vectors stored in Endee")

    duration = time.time() - t0
    return {
        "docs":     len(docs),
        "chunks":   len(chunks),
        "stored":   stored,
        "duration": duration,
    }


def _print_chunk_preview(chunks):
    table = Table(title="Chunk Preview (first 10)", show_header=True,
                  header_style="bold cyan")
    table.add_column("ID",       style="dim",  width=20, overflow="fold")
    table.add_column("Type",     style="cyan", width=18)
    table.add_column("Source",   style="dim",  overflow="fold", max_width=35)
    table.add_column("Text snippet", overflow="fold")

    for c in chunks:
        snippet = c.text[:80].replace("\n", " ") + "…"
        table.add_row(c.id, c.doc_type, c.source, snippet)

    console.print(table)


def print_summary(result: dict):
    console.print(f"""
[bold green]
╔══════════════════════════════════╗
║      Indexing Complete  ✅       ║
╚══════════════════════════════════╝[/]

  Documents collected : [bold]{result["docs"]}[/]
  Chunks generated    : [bold]{result["chunks"]}[/]
  Vectors stored      : [bold]{result["stored"]}[/]
  Duration            : [bold]{result["duration"]:.1f}s[/]
  {'[yellow](DRY RUN – nothing written)[/]' if result.get("dry_run") else ''}

[dim]You can now run:[/]
  [cyan]python -m cli.sensei_cli ask "How do I start my project?"[/]
  [cyan]python -m cli.sensei_cli chat[/]
  [cyan]uvicorn api.server:app --port 8000[/]
""")


# ── Argument parser ────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(
        description="Index your WSL + Windows developer environment into Endee.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python scripts/index_environment.py
  python scripts/index_environment.py --sources bash_history config_files
  python scripts/index_environment.py --rebuild
  python scripts/index_environment.py --sample
  python scripts/index_environment.py --dry-run
""",
    )
    parser.add_argument(
        "--sources", nargs="+",
        default=["bash_history", "powershell_history", "config_files"],
        choices=["bash_history", "powershell_history", "config_files"],
        help="Which collectors to run (default: all).",
    )
    parser.add_argument(
        "--rebuild", action="store_true",
        help="Delete the existing Endee index before re-indexing.",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Collect and chunk but do NOT write to Endee.",
    )
    parser.add_argument(
        "--sample", action="store_true",
        help="Use bundled sample data instead of live environment files.",
    )
    return parser.parse_args()


# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    args = parse_args()
    banner()

    result = run_indexing(
        sources=args.sources,
        force_rebuild=args.rebuild,
        dry_run=args.dry_run,
        use_sample=args.sample,
    )

    print_summary(result)
