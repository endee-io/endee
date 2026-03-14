"""
cli/sensei_cli.py
──────────────────
Rich interactive CLI for WSL Sensei.

Commands
────────
  sensei ask   "<question>"   – Ask a question (full RAG pipeline)
  sensei search "<query>"     – Semantic search only (no LLM)
  sensei index                – Re-index the development environment
  sensei status               – Show Endee index statistics
  sensei chat                 – Start an interactive REPL session

Usage examples:
  python -m cli.sensei_cli ask "How do I start my project?"
  python -m cli.sensei_cli search "nginx config" --top-k 5
  python -m cli.sensei_cli index --sources bash_history config_files
  python -m cli.sensei_cli status
  python -m cli.sensei_cli chat
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import click
from rich import print as rprint
from rich.console import Console
from rich.markdown import Markdown
from rich.panel import Panel
from rich.progress import Progress, SpinnerColumn, TextColumn
from rich.table import Table
from rich.theme import Theme

# ── Console setup ─────────────────────────────────────────────────────────────

THEME = Theme({
    "info":    "bold cyan",
    "success": "bold green",
    "warning": "bold yellow",
    "error":   "bold red",
    "dim":     "dim white",
    "score":   "bold magenta",
})

console = Console(theme=THEME)

BANNER = r"""
 __      _____ _      ___
 \ \    / / __| |    / __|___ _ _  ___ ___ ___
  \ \/\/ /\__ \ |__ \__ \/ -_) ' \(_-</ -_) |
   \_/\_/ |___/____||___/\___|_||_/__/\___|_|
  AI Assistant for Windows + WSL Development
  Powered by Endee Vector Database
"""


# ── Shared helpers ────────────────────────────────────────────────────────────

def _print_banner():
    console.print(Panel(BANNER, style="bold blue", expand=False))


def _print_sources(sources, max_shown: int = 3):
    if not sources:
        return
    table = Table(
        title="📂 Retrieved Sources",
        show_header=True,
        header_style="bold cyan",
        expand=True,
    )
    table.add_column("#",        style="dim",  width=3)
    table.add_column("Score",    style="score", width=6)
    table.add_column("Type",     style="info",  width=16)
    table.add_column("Source",   style="dim",   overflow="fold")
    table.add_column("Snippet",  overflow="fold")

    for i, src in enumerate(sources[:max_shown], 1):
        snippet = src.get("text", "")[:80].replace("\n", " ") + "…"
        table.add_row(
            str(i),
            f"{src.get('score', 0):.2f}",
            src.get("doc_type", "?"),
            src.get("source", "?"),
            snippet,
        )

    console.print(table)


def _spinner(message: str):
    return Progress(
        SpinnerColumn(),
        TextColumn(f"[info]{message}[/]"),
        transient=True,
    )


# ── CLI group ─────────────────────────────────────────────────────────────────

@click.group()
@click.version_option("1.0.0", prog_name="WSL Sensei")
def cli():
    """WSL Sensei – AI assistant for your Windows + WSL dev environment."""


# ── ask ───────────────────────────────────────────────────────────────────────

@cli.command()
@click.argument("question")
@click.option("--top-k",    default=6,      show_default=True,
              help="Number of chunks to retrieve from Endee.")
@click.option("--backend",  default=None,   help="LLM backend: ollama|openai|anthropic|mock")
@click.option("--min-score", default=0.1,   show_default=True,
              help="Minimum cosine similarity to include a chunk.")
@click.option("--show-context", is_flag=True, default=False,
              help="Print the raw context passed to the LLM.")
@click.option("--sources",  default=3,      show_default=True,
              help="Number of source chunks to display.")
def ask(question, top_k, backend, min_score, show_context, sources):
    """Ask a question using the full RAG pipeline.

    \b
    Examples:
      sensei ask "How do I start my project?"
      sensei ask "Where is the nginx config?" --top-k 8
      sensei ask "Which command installs dependencies?" --backend mock
    """
    from config import LLM_BACKEND
    from rag.rag_pipeline import ask as rag_ask

    _backend = backend or LLM_BACKEND

    console.print(f"\n[info]❓ Question:[/] {question}")
    console.print(f"[dim]   backend={_backend}  top_k={top_k}  min_score={min_score}[/]\n")

    with _spinner(f"Searching Endee + generating answer ({_backend}) …"):
        t0     = time.time()
        result = rag_ask(
            question=question,
            top_k=top_k,
            backend=_backend,
            min_score=min_score,
        )

    console.print(
        Panel(
            Markdown(result.answer),
            title="🧠 WSL Sensei Answer",
            title_align="left",
            style="green",
            padding=(1, 2),
        )
    )

    # Sources table
    _print_sources(
        [
            {"score": s.score, "doc_type": s.doc_type,
             "source": s.source, "text": s.text}
            for s in result.sources
        ],
        max_shown=sources,
    )

    if show_context:
        console.print(Panel(result.context, title="📋 Context", style="dim"))

    console.print(
        f"\n[dim]⏱  {result.latency_ms:.0f} ms  |  "
        f"{len(result.sources)} chunks retrieved  |  backend: {result.backend}[/]\n"
    )


# ── search ────────────────────────────────────────────────────────────────────

@cli.command()
@click.argument("query")
@click.option("--top-k",       default=6,    show_default=True)
@click.option("--filter-type", default=None,
              help="Filter by doc type: bash_history|config|script|log")
@click.option("--min-score",   default=0.0,  show_default=True)
def search(query, top_k, filter_type, min_score):
    """Semantic search only – no LLM generation.

    \b
    Examples:
      sensei search "nginx config"
      sensei search "npm install" --filter-type bash_history
      sensei search "port 3000" --top-k 10
    """
    from retrieval.semantic_search import semantic_search

    console.print(f"\n[info]🔍 Searching:[/] {query}\n")

    with _spinner("Querying Endee …"):
        t0      = time.time()
        results = semantic_search(
            query=query,
            top_k=top_k,
            filter_type=filter_type,
            min_score=min_score,
        )
        elapsed = (time.time() - t0) * 1000

    if not results:
        console.print("[warning]No results found. Have you run `sensei index` yet?[/]")
        return

    table = Table(
        title=f"🔎 Search Results for: '{query}'",
        show_header=True,
        header_style="bold cyan",
        expand=True,
    )
    table.add_column("#",        style="dim",    width=3)
    table.add_column("Score",    style="score",  width=6)
    table.add_column("Type",     style="info",   width=16)
    table.add_column("Source",   style="dim",    overflow="fold", max_width=40)
    table.add_column("Text",     overflow="fold")

    for i, r in enumerate(results, 1):
        snippet = r.text[:120].replace("\n", " ") + ("…" if len(r.text) > 120 else "")
        table.add_row(str(i), f"{r.score:.3f}", r.doc_type, r.source, snippet)

    console.print(table)
    console.print(f"\n[dim]⏱  {elapsed:.0f} ms  |  {len(results)} results[/]\n")


# ── index ─────────────────────────────────────────────────────────────────────

@cli.command()
@click.option(
    "--sources",
    default="bash_history,powershell_history,config_files",
    show_default=True,
    help="Comma-separated list of collectors to run.",
)
@click.option(
    "--force-rebuild", is_flag=True, default=False,
    help="Delete and recreate the Endee index from scratch.",
)
def index(sources, force_rebuild):
    """Index your development environment into Endee.

    \b
    Examples:
      sensei index
      sensei index --sources bash_history,config_files
      sensei index --force-rebuild
    """
    from collectors.bash_history_collector       import collect_bash_history
    from collectors.powershell_history_collector import collect_powershell_history
    from collectors.config_file_scanner          import collect_config_files
    from indexing.chunker                        import chunk_documents
    from indexing.embed_store                    import store_chunks, delete_index

    source_list = [s.strip() for s in sources.split(",") if s.strip()]

    _print_banner()
    console.print(f"[info]📦 Indexing sources:[/] {', '.join(source_list)}")
    if force_rebuild:
        console.print("[warning]⚠  Force rebuild: existing index will be deleted.[/]")

    t0 = time.time()

    if force_rebuild:
        with _spinner("Deleting existing index …"):
            delete_index()

    docs = []
    with _spinner("Collecting documents …"):
        if "bash_history"       in source_list: docs += collect_bash_history()
        if "powershell_history" in source_list: docs += collect_powershell_history()
        if "config_files"       in source_list: docs += collect_config_files()

    console.print(f"[success]✓[/]  Collected [bold]{len(docs)}[/] documents")

    with _spinner("Chunking documents …"):
        chunks = chunk_documents(docs)

    console.print(f"[success]✓[/]  Generated [bold]{len(chunks)}[/] chunks")

    with _spinner(f"Embedding + storing in Endee (this may take a minute) …"):
        stored = store_chunks(chunks)

    duration = time.time() - t0
    console.print(
        Panel(
            f"[success]✅  Indexing complete![/]\n\n"
            f"  Documents : [bold]{len(docs)}[/]\n"
            f"  Chunks    : [bold]{len(chunks)}[/]\n"
            f"  Stored    : [bold]{stored}[/] vectors in Endee\n"
            f"  Duration  : [bold]{duration:.1f}s[/]",
            title="Index Report",
            style="green",
        )
    )


# ── status ────────────────────────────────────────────────────────────────────

@cli.command()
def status():
    """Show Endee index statistics and configuration."""
    from indexing.embed_store import get_index_stats
    from config import (
        ENDEE_BASE_URL, ENDEE_INDEX_NAME,
        EMBEDDING_MODEL, EMBEDDING_DIMENSION, LLM_BACKEND,
    )

    _print_banner()

    with _spinner("Fetching status …"):
        stats = get_index_stats()

    table = Table(title="⚙  WSL Sensei Configuration", show_header=False, expand=False)
    table.add_column("Key",   style="info")
    table.add_column("Value", style="bold")

    table.add_row("Endee URL",         ENDEE_BASE_URL)
    table.add_row("Index name",        ENDEE_INDEX_NAME)
    table.add_row("Embedding model",   EMBEDDING_MODEL)
    table.add_row("Embedding dim",     str(EMBEDDING_DIMENSION))
    table.add_row("LLM backend",       LLM_BACKEND)

    if "error" in stats:
        table.add_row("Endee status",  f"[error]{stats['error']}[/]")
    else:
        table.add_row("Vectors stored", str(stats.get("count", "?")))
        table.add_row("Space type",     stats.get("space", "?"))

    console.print(table)


# ── chat REPL ─────────────────────────────────────────────────────────────────

@cli.command()
@click.option("--backend", default=None, help="LLM backend override.")
@click.option("--top-k",   default=6,    show_default=True)
def chat(backend, top_k):
    """Start an interactive chat session with WSL Sensei.

    Type 'exit' or press Ctrl-C to quit.
    Type '/search <query>' for raw search results.
    Type '/status' to check the index.
    """
    from config import LLM_BACKEND
    from rag.rag_pipeline import ask as rag_ask
    from retrieval.semantic_search import semantic_search

    _backend = backend or LLM_BACKEND

    _print_banner()
    console.print(
        Panel(
            "[bold]Welcome to WSL Sensei Interactive Chat[/]\n\n"
            "  Ask anything about your development environment.\n"
            "  Type [bold cyan]/search <query>[/] for raw vector search.\n"
            "  Type [bold cyan]/status[/] for index info.\n"
            "  Type [bold red]exit[/] or press [bold]Ctrl-C[/] to quit.",
            style="blue",
            expand=False,
        )
    )

    history: list[str] = []

    while True:
        try:
            user_input = console.input("\n[bold cyan]You ▶[/] ").strip()
        except (KeyboardInterrupt, EOFError):
            console.print("\n[dim]Goodbye! 👋[/]")
            break

        if not user_input:
            continue
        if user_input.lower() in {"exit", "quit", "q"}:
            console.print("[dim]Goodbye! 👋[/]")
            break

        # ── slash commands ──────────────────────────────────────────────────
        if user_input.startswith("/search "):
            raw_query = user_input[8:].strip()
            results   = semantic_search(raw_query, top_k=top_k)
            _print_sources(
                [{"score": r.score, "doc_type": r.doc_type,
                  "source": r.source, "text": r.text}
                 for r in results]
            )
            continue

        if user_input.strip() == "/status":
            from indexing.embed_store import get_index_stats
            stats = get_index_stats()
            console.print(stats)
            continue

        # ── full RAG ────────────────────────────────────────────────────────
        history.append(user_input)

        with _spinner("Thinking …"):
            result = rag_ask(question=user_input, top_k=top_k, backend=_backend)

        console.print(
            Panel(
                Markdown(result.answer),
                title="🧠 Sensei",
                title_align="left",
                style="green",
                padding=(1, 2),
            )
        )

        _print_sources(
            [{"score": s.score, "doc_type": s.doc_type,
              "source": s.source, "text": s.text}
             for s in result.sources],
            max_shown=3,
        )

        console.print(
            f"[dim]  ⏱ {result.latency_ms:.0f} ms  |  "
            f"{len(result.sources)} chunks  |  {result.backend}[/]"
        )


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    cli()
