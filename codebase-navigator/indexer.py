"""
Codebase Indexer

The main pipeline that:
1. Scans a codebase directory
2. Parses code into chunks
3. Generates embeddings
4. Stores everything in Endee
"""

from pathlib import Path
from typing import Optional
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn, BarColumn, TaskProgressColumn

from config import config
from parser import CodeParser, CodeChunk, parse_codebase
from embeddings import EmbeddingGenerator, SparseVectorGenerator
from endee_client import EndeeClient, EndeeError


console = Console()


class CodebaseIndexer:
    """
    Main indexer class that orchestrates the indexing pipeline.
    
    Usage:
        indexer = CodebaseIndexer()
        indexer.index_directory("/path/to/codebase")
    """
    
    def __init__(
        self,
        index_name: Optional[str] = None,
        endee_client: Optional[EndeeClient] = None,
        embedding_generator: Optional[EmbeddingGenerator] = None,
    ):
        """
        Initialize the indexer.
        
        Args:
            index_name: Name for the Endee index (default: from config)
            endee_client: Custom Endee client (default: creates new one)
            embedding_generator: Custom embedding generator (default: creates new one)
        """
        self.index_name = index_name or config.INDEX_NAME
        self.client = endee_client or EndeeClient()
        self.embedder = embedding_generator or EmbeddingGenerator()
        self.sparse_gen = SparseVectorGenerator()
        self.parser = CodeParser()
    
    def index_directory(
        self,
        directory: str,
        exclude_dirs: Optional[list[str]] = None,
        recreate_index: bool = False,
        batch_size: int = 50,
    ) -> dict:
        """
        Index an entire codebase directory.
        
        Args:
            directory: Path to the codebase root
            exclude_dirs: Directories to skip
            recreate_index: If True, delete existing index first
            batch_size: Number of chunks to process at once
        
        Returns:
            Stats dict with counts and timings
        """
        directory_path = Path(directory).resolve()
        
        if not directory_path.exists():
            raise ValueError(f"Directory not found: {directory_path}")
        
        console.print(f"\n[bold blue]🔍 Indexing codebase:[/] {directory_path}\n")
        
        # Step 1: Check/Create index
        self._ensure_index(recreate_index)
        
        # Step 2: Collect all chunks
        console.print("[yellow]📁 Scanning files...[/]")
        chunks = list(parse_codebase(str(directory_path), exclude_dirs))
        
        if not chunks:
            console.print("[red]No code files found![/]")
            return {"files": 0, "chunks": 0}
        
        console.print(f"[green]Found {len(chunks)} chunks from codebase[/]\n")
        
        # Step 3: Generate embeddings and index
        stats = self._index_chunks(chunks, batch_size)
        
        # Summary
        console.print("\n[bold green]✅ Indexing complete![/]")
        console.print(f"   • Chunks indexed: {stats['indexed']}")
        console.print(f"   • Tokens used: {self.embedder.total_tokens:,}")
        console.print(f"   • Estimated cost: ${self.embedder.estimate_cost():.4f}")
        
        return stats
    
    def _ensure_index(self, recreate: bool = False):
        """Ensure the index exists, optionally recreating it."""
        
        # Check server health
        if not self.client.health_check():
            raise EndeeError(
                f"Cannot connect to Endee server at {self.client.base_url}. "
                "Make sure the server is running."
            )
        
        exists = self.client.index_exists(self.index_name)
        
        if exists and recreate:
            console.print(f"[yellow]Deleting existing index: {self.index_name}[/]")
            self.client.delete_index(self.index_name)
            exists = False
        
        if not exists:
            console.print(f"[yellow]Creating index: {self.index_name}[/]")
            self.client.create_index(
                name=self.index_name,
                dimensions=self.embedder.get_dimensions(),
                metric="cosine",
                quantization="int8",
            )
            console.print(f"[green]Index created successfully[/]")
        else:
            console.print(f"[green]Using existing index: {self.index_name}[/]")
    
    def _index_chunks(self, chunks: list[CodeChunk], batch_size: int) -> dict:
        """Generate embeddings and insert chunks into Endee."""
        
        total = len(chunks)
        indexed = 0
        errors = 0
        
        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TaskProgressColumn(),
            console=console,
        ) as progress:
            task = progress.add_task("[cyan]Indexing...", total=total)
            
            # Process in batches
            for i in range(0, total, batch_size):
                batch = chunks[i:i + batch_size]
                
                try:
                    # Prepare texts for embedding
                    texts = [self._prepare_text(chunk) for chunk in batch]
                    
                    # Generate embeddings
                    embeddings = self.embedder.embed_batch(texts)
                    
                    # Prepare items for Endee
                    items = []
                    for chunk, embedding in zip(batch, embeddings):
                        items.append({
                            "id": chunk.id,
                            "vector": embedding,
                            "metadata": chunk.to_dict(),
                        })
                    
                    # Insert into Endee
                    self.client.insert_batch(self.index_name, items)
                    
                    indexed += len(batch)
                    
                except Exception as e:
                    console.print(f"\n[red]Error indexing batch: {e}[/]")
                    errors += len(batch)
                
                progress.update(task, completed=i + len(batch))
        
        return {
            "indexed": indexed,
            "errors": errors,
            "total": total,
        }
    
    def _prepare_text(self, chunk: CodeChunk) -> str:
        """
        Prepare chunk text for embedding.
        
        Adds context like file path and language for better semantic understanding.
        """
        context_parts = [
            f"File: {chunk.file_path}",
            f"Language: {chunk.language}",
        ]
        
        if chunk.name:
            context_parts.append(f"Name: {chunk.name}")
        
        if chunk.chunk_type != "block":
            context_parts.append(f"Type: {chunk.chunk_type}")
        
        context = " | ".join(context_parts)
        
        return f"{context}\n\n{chunk.content}"
    
    def add_file(self, file_path: str) -> int:
        """
        Add or update a single file in the index.
        
        Args:
            file_path: Path to the file
        
        Returns:
            Number of chunks indexed
        """
        path = Path(file_path)
        
        if not path.exists():
            raise ValueError(f"File not found: {file_path}")
        
        chunks = self.parser.parse_file(path)
        
        if not chunks:
            return 0
        
        # Generate embeddings
        texts = [self._prepare_text(chunk) for chunk in chunks]
        embeddings = self.embedder.embed_batch(texts)
        
        # Prepare and insert
        items = []
        for chunk, embedding in zip(chunks, embeddings):
            items.append({
                "id": chunk.id,
                "vector": embedding,
                "metadata": chunk.to_dict(),
            })
        
        self.client.insert_batch(self.index_name, items)
        
        return len(chunks)
    
    def remove_file(self, file_path: str) -> int:
        """
        Remove all chunks from a file.
        
        Note: This is a best-effort operation since we need to know
        the chunk IDs. Consider tracking indexed files separately.
        """
        # This would require listing all vectors and filtering by file_path
        # For now, log a warning
        console.print(
            f"[yellow]Warning: Removing file chunks requires manual ID tracking. "
            f"Consider re-indexing with recreate_index=True[/]"
        )
        return 0


def index_codebase(
    directory: str,
    index_name: Optional[str] = None,
    exclude_dirs: Optional[list[str]] = None,
    recreate: bool = False,
) -> dict:
    """
    Convenience function to index a codebase.
    
    Args:
        directory: Path to codebase root
        index_name: Name for the index
        exclude_dirs: Directories to skip
        recreate: Whether to recreate the index
    
    Returns:
        Stats dict
    """
    indexer = CodebaseIndexer(index_name=index_name)
    return indexer.index_directory(
        directory,
        exclude_dirs=exclude_dirs,
        recreate_index=recreate,
    )
