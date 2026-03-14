#!/usr/bin/env python3
"""
AI Codebase Navigator - CLI Interface

A command-line tool for indexing and searching codebases
using semantic vector search powered by Endee.

Usage:
    python cli.py index /path/to/codebase
    python cli.py search "authentication logic"
    python cli.py ask "how does the login system work?"
"""

import typer
from typing import Optional
from pathlib import Path
from rich.console import Console
from rich.table import Table
from rich.panel import Panel
from rich.markdown import Markdown
from rich.syntax import Syntax

from config import config
from indexer import CodebaseIndexer
from search import SearchEngine
from endee_client import EndeeClient, EndeeError


# Initialize Typer app
app = typer.Typer(
    name="codebase-navigator",
    help="AI-powered codebase search and navigation",
    add_completion=False,
)

console = Console()


# ==================== Index Commands ====================

@app.command()
def index(
    directory: str = typer.Argument(..., help="Path to the codebase directory"),
    name: Optional[str] = typer.Option(None, "--name", "-n", help="Index name"),
    recreate: bool = typer.Option(False, "--recreate", "-r", help="Delete and recreate index"),
    exclude: Optional[str] = typer.Option(None, "--exclude", "-e", help="Comma-separated dirs to exclude"),
):
    """
    Index a codebase directory.
    
    Example:
        python cli.py index ./my-project --name my-project-index
    """
    # Validate config
    missing = config.validate(require_llm=False)
    if missing:
        console.print(f"[red]Missing configuration: {', '.join(missing)}[/]")
        console.print("Copy .env.example to .env and fill in the values.")
        raise typer.Exit(1)
    
    # Parse exclude dirs
    exclude_dirs = [d.strip() for d in exclude.split(",")] if exclude else None
    
    try:
        indexer = CodebaseIndexer(index_name=name)
        stats = indexer.index_directory(
            directory=directory,
            exclude_dirs=exclude_dirs,
            recreate_index=recreate,
        )
    except EndeeError as e:
        console.print(f"[red]Endee Error: {e}[/]")
        raise typer.Exit(1)
    except Exception as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


@app.command()
def status():
    """Show index status and server health."""
    try:
        client = EndeeClient()
        
        if client.health_check():
            console.print("[green]✓ Endee server is running[/]")
        else:
            console.print(f"[red]✗ Cannot connect to Endee at {client.base_url}[/]")
            raise typer.Exit(1)
        
        # List indices
        indices = client.list_indices()
        
        if indices:
            table = Table(title="Available Indices")
            table.add_column("Name", style="cyan")
            table.add_column("Vectors", justify="right")
            table.add_column("Dimensions", justify="right")
            
            for idx in indices:
                table.add_row(
                    idx.get("name", ""),
                    str(idx.get("vector_count", "?")),
                    str(idx.get("dimensions", "?")),
                )
            
            console.print(table)
        else:
            console.print("[yellow]No indices found. Run 'index' to create one.[/]")
            
    except EndeeError as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


# ==================== Search Commands ====================

@app.command()
def search(
    query: str = typer.Argument(..., help="Search query"),
    top_k: int = typer.Option(10, "--top-k", "-k", help="Number of results"),
    language: Optional[str] = typer.Option(None, "--language", "-l", help="Filter by language"),
    path: Optional[str] = typer.Option(None, "--path", "-p", help="Filter by file path pattern"),
    index_name: Optional[str] = typer.Option(None, "--index", "-i", help="Index name"),
):
    """
    Search the codebase with natural language.
    
    Examples:
        python cli.py search "authentication logic"
        python cli.py search "database connection" --language python
        python cli.py search "api endpoints" --path "src/routes"
    """
    try:
        engine = SearchEngine(index_name=index_name)
        results = engine.search(
            query=query,
            top_k=top_k,
            language=language,
            file_pattern=path,
        )
        
        if not results:
            console.print("[yellow]No results found.[/]")
            return
        
        # Display results
        console.print(f"\n[bold]Found {len(results)} results for:[/] {query}\n")
        
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("#", style="dim", width=3)
        table.add_column("Location", style="green")
        table.add_column("Type", width=10)
        table.add_column("Name", style="yellow")
        table.add_column("Score", justify="right", width=8)
        
        for i, r in enumerate(results, 1):
            table.add_row(
                str(i),
                f"{r.file_path}:{r.start_line}",
                r.chunk_type,
                r.name or "-",
                f"{r.score:.3f}",
            )
        
        console.print(table)
        console.print("\n[dim]Tip: Use 'show <number>' to view the code[/]")
        
    except EndeeError as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


@app.command()
def ask(
    question: str = typer.Argument(..., help="Question about the codebase"),
    top_k: int = typer.Option(5, "--top-k", "-k", help="Context chunks to retrieve"),
    language: Optional[str] = typer.Option(None, "--language", "-l", help="Filter by language"),
    index_name: Optional[str] = typer.Option(None, "--index", "-i", help="Index name"),
    show_sources: bool = typer.Option(False, "--sources", "-s", help="Show source locations"),
):
    """
    Ask a question about the codebase (RAG).
    
    Examples:
        python cli.py ask "how does user authentication work?"
        python cli.py ask "explain the database schema" --sources
    """
    missing = config.validate(require_llm=True)
    if missing:
        console.print(f"[red]Missing configuration: {', '.join(missing)}[/]")
        raise typer.Exit(1)
    
    try:
        engine = SearchEngine(index_name=index_name)
        
        with console.status("[cyan]Thinking...[/]"):
            result = engine.ask(
                question=question,
                top_k=top_k,
                language=language,
            )
        
        # Display answer
        console.print(Panel(
            Markdown(result["answer"]),
            title="[bold cyan]Answer[/]",
            border_style="cyan",
        ))
        
        if show_sources and result["sources"]:
            console.print("\n[bold]Sources:[/]")
            for source in result["sources"]:
                console.print(f"  • {source}")
                
    except ValueError as e:
        console.print(f"[red]{e}[/]")
        raise typer.Exit(1)
    except EndeeError as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


@app.command()
def similar(
    file_path: str = typer.Argument(..., help="Path to the code file"),
    start_line: int = typer.Option(1, "--start", "-s", help="Start line"),
    end_line: int = typer.Option(None, "--end", "-e", help="End line"),
    top_k: int = typer.Option(5, "--top-k", "-k", help="Number of results"),
    index_name: Optional[str] = typer.Option(None, "--index", "-i", help="Index name"),
):
    """
    Find code similar to a snippet from a file.
    
    Example:
        python cli.py similar src/auth.py --start 10 --end 30
    """
    try:
        # Read the code snippet
        path = Path(file_path)
        if not path.exists():
            console.print(f"[red]File not found: {file_path}[/]")
            raise typer.Exit(1)
        
        with open(path, "r", encoding="utf-8") as f:
            lines = f.readlines()
        
        end_line = end_line or len(lines)
        snippet = "".join(lines[start_line - 1:end_line])
        
        engine = SearchEngine(index_name=index_name)
        results = engine.search_similar(
            code_snippet=snippet,
            top_k=top_k,
            exclude_file=str(path.resolve()),
        )
        
        if not results:
            console.print("[yellow]No similar code found.[/]")
            return
        
        console.print(f"\n[bold]Found {len(results)} similar code sections:[/]\n")
        
        for i, r in enumerate(results, 1):
            console.print(f"[cyan]{i}.[/] {r.location()} ({r.language})")
            console.print(f"   [dim]Score: {r.score:.3f} | Type: {r.chunk_type}[/]\n")
            
    except EndeeError as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


@app.command()
def explain(
    file_path: str = typer.Argument(..., help="Path to the code file"),
    start_line: int = typer.Option(1, "--start", "-s", help="Start line"),
    end_line: int = typer.Option(None, "--end", "-e", help="End line"),
    index_name: Optional[str] = typer.Option(None, "--index", "-i", help="Index name"),
):
    """
    Explain a section of code.
    
    Example:
        python cli.py explain src/auth.py --start 10 --end 50
    """
    missing = config.validate(require_llm=True)
    if missing:
        console.print(f"[red]Missing configuration: {', '.join(missing)}[/]")
        raise typer.Exit(1)
    
    try:
        path = Path(file_path)
        if not path.exists():
            console.print(f"[red]File not found: {file_path}[/]")
            raise typer.Exit(1)
        
        # Get line count if end not specified
        if end_line is None:
            with open(path, "r") as f:
                end_line = sum(1 for _ in f)
        
        engine = SearchEngine(index_name=index_name)
        
        with console.status("[cyan]Analyzing code...[/]"):
            explanation = engine.explain(str(path), start_line, end_line)
        
        console.print(Panel(
            Markdown(explanation),
            title=f"[bold cyan]Explanation: {file_path}:{start_line}-{end_line}[/]",
            border_style="cyan",
        ))
        
    except ValueError as e:
        console.print(f"[red]{e}[/]")
        raise typer.Exit(1)


# ==================== Utility Commands ====================

@app.command()
def show(
    file_path: str = typer.Argument(..., help="File path"),
    start_line: int = typer.Option(1, "--start", "-s", help="Start line"),
    end_line: int = typer.Option(None, "--end", "-e", help="End line"),
):
    """
    Display code from a file with syntax highlighting.
    
    Example:
        python cli.py show src/auth.py --start 10 --end 30
    """
    path = Path(file_path)
    if not path.exists():
        console.print(f"[red]File not found: {file_path}[/]")
        raise typer.Exit(1)
    
    try:
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
        
        # Determine language from extension
        ext_to_lang = {
            ".py": "python",
            ".js": "javascript",
            ".ts": "typescript",
            ".tsx": "typescript",
            ".jsx": "javascript",
            ".cpp": "cpp",
            ".c": "c",
            ".h": "c",
            ".hpp": "cpp",
            ".java": "java",
            ".go": "go",
            ".rs": "rust",
        }
        lang = ext_to_lang.get(path.suffix, "text")
        
        syntax = Syntax(
            content,
            lang,
            theme="monokai",
            line_numbers=True,
            line_range=(start_line, end_line),
        )
        
        console.print(syntax)
        
    except Exception as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


@app.command()
def delete_index(
    name: str = typer.Argument(..., help="Index name to delete"),
    force: bool = typer.Option(False, "--force", "-f", help="Skip confirmation"),
):
    """Delete an index."""
    if not force:
        confirm = typer.confirm(f"Are you sure you want to delete index '{name}'?")
        if not confirm:
            raise typer.Abort()
    
    try:
        client = EndeeClient()
        client.delete_index(name)
        console.print(f"[green]Index '{name}' deleted successfully.[/]")
    except EndeeError as e:
        console.print(f"[red]Error: {e}[/]")
        raise typer.Exit(1)


# ==================== Entry Point ====================

if __name__ == "__main__":
    app()
