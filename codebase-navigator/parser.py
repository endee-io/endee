"""
Code Parser and Chunker

This module handles:
1. Scanning a directory for code files
2. Parsing code into meaningful chunks (functions, classes, etc.)
3. Creating structured metadata for each chunk
"""

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Generator, Optional

from config import config


@dataclass
class CodeChunk:
    """Represents a chunk of code with its metadata."""
    
    # Unique identifier: "filepath:start_line-end_line"
    id: str
    
    # The actual code content
    content: str
    
    # Metadata for filtering
    file_path: str
    language: str
    start_line: int
    end_line: int
    chunk_type: str  # "function", "class", "module", "block"
    name: str = ""   # Function/class name if applicable
    
    # Additional context
    imports: list[str] = field(default_factory=list)
    
    def to_dict(self) -> dict:
        """Convert to dictionary for Endee metadata."""
        return {
            "file_path": self.file_path,
            "language": self.language,
            "start_line": self.start_line,
            "end_line": self.end_line,
            "chunk_type": self.chunk_type,
            "name": self.name,
        }


class CodeParser:
    """
    Parses source code files into chunks.
    
    Uses simple regex-based parsing for broader language support.
    For production, consider using tree-sitter for AST-based parsing.
    """
    
    def __init__(self, max_chunk_size: Optional[int] = None, chunk_overlap: Optional[int] = None):
        self.max_chunk_size = max_chunk_size or config.MAX_CHUNK_SIZE
        self.chunk_overlap = chunk_overlap or config.CHUNK_OVERLAP
        
        # Patterns for detecting code structures
        self.patterns = {
            "python": {
                "function": r"^(\s*)(async\s+)?def\s+(\w+)\s*\(",
                "class": r"^(\s*)class\s+(\w+)",
                "import": r"^(?:from\s+\S+\s+)?import\s+.+",
            },
            "javascript": {
                "function": r"^(\s*)(?:async\s+)?function\s+(\w+)|^(\s*)(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s+)?\(?.*\)?\s*=>",
                "class": r"^(\s*)class\s+(\w+)",
                "import": r"^import\s+.+|^const\s+.+\s*=\s*require\s*\(",
            },
            "typescript": {
                "function": r"^(\s*)(?:async\s+)?function\s+(\w+)|^(\s*)(?:export\s+)?(?:const|let|var)\s+(\w+)\s*(?::\s*\w+)?\s*=\s*(?:async\s+)?\(?.*\)?\s*=>",
                "class": r"^(\s*)(?:export\s+)?class\s+(\w+)",
                "import": r"^import\s+.+",
            },
            "cpp": {
                "function": r"^(\s*)(?:\w+\s+)+(\w+)\s*\([^)]*\)\s*(?:const)?\s*\{?",
                "class": r"^(\s*)class\s+(\w+)",
                "import": r"^#include\s+.+",
            },
        }
    
    def scan_directory(self, directory: str, exclude_dirs: Optional[list[str]] = None) -> Generator[Path, None, None]:
        """
        Recursively scan a directory for supported code files.
        
        Args:
            directory: Root directory to scan
            exclude_dirs: Directory names to skip (e.g., ["node_modules", ".git"])
        
        Yields:
            Path objects for each supported file
        """
        exclude_dirs = exclude_dirs or [
            "node_modules", ".git", "__pycache__", ".venv", "venv",
            "dist", "build", ".next", "target", "bin", "obj"
        ]
        
        root = Path(directory)
        
        for path in root.rglob("*"):
            # Skip excluded directories
            if any(excluded in path.parts for excluded in exclude_dirs):
                continue
            
            # Only yield supported files
            if path.is_file() and path.suffix in config.SUPPORTED_EXTENSIONS:
                yield path
    
    def parse_file(self, file_path: Path) -> list[CodeChunk]:
        """
        Parse a single file into chunks.
        
        Args:
            file_path: Path to the code file
        
        Returns:
            List of CodeChunk objects
        """
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception as e:
            print(f"Error reading {file_path}: {e}")
            return []
        
        language = config.SUPPORTED_EXTENSIONS.get(file_path.suffix, "text")
        relative_path = str(file_path)
        
        # For small files, treat as single chunk
        if len(content) <= self.max_chunk_size:
            return [CodeChunk(
                id=f"{relative_path}:1-{content.count(chr(10)) + 1}",
                content=content,
                file_path=relative_path,
                language=language,
                start_line=1,
                end_line=content.count("\n") + 1,
                chunk_type="module",
                name=file_path.stem,
                imports=self._extract_imports(content, language),
            )]
        
        # For larger files, split into logical chunks
        return self._split_into_chunks(content, relative_path, language)
    
    def _split_into_chunks(self, content: str, file_path: str, language: str) -> list[CodeChunk]:
        """Split large files into logical chunks."""
        chunks = []
        lines = content.split("\n")
        
        # Try to find logical boundaries (functions, classes)
        boundaries = self._find_boundaries(lines, language)
        
        if not boundaries:
            # Fall back to simple line-based chunking
            return self._simple_chunk(content, file_path, language)
        
        # Create chunks based on boundaries
        imports = self._extract_imports(content, language)
        
        for i, (start_line, end_line, chunk_type, name) in enumerate(boundaries):
            chunk_content = "\n".join(lines[start_line - 1:end_line])
            
            # Skip very small chunks
            if len(chunk_content.strip()) < 50:
                continue
            
            chunks.append(CodeChunk(
                id=f"{file_path}:{start_line}-{end_line}",
                content=chunk_content,
                file_path=file_path,
                language=language,
                start_line=start_line,
                end_line=end_line,
                chunk_type=chunk_type,
                name=name,
                imports=imports if i == 0 else [],
            ))
        
        return chunks if chunks else self._simple_chunk(content, file_path, language)
    
    def _find_boundaries(self, lines: list[str], language: str) -> list[tuple]:
        """
        Find logical boundaries in code (functions, classes).
        
        Returns:
            List of (start_line, end_line, type, name) tuples
        """
        patterns = self.patterns.get(language, {})
        if not patterns:
            return []
        
        boundaries = []
        current_block = None
        for i, line in enumerate(lines, 1):
            # Check for function definition
            func_pattern = patterns.get("function")
            if func_pattern:
                match = re.match(func_pattern, line)
                if match:
                    # Save previous block
                    if current_block:
                        current_block = (current_block[0], i - 1, current_block[2], current_block[3])
                        if current_block[1] >= current_block[0]:
                            boundaries.append(current_block)
                    
                    # Extract name from match groups
                    name = ""
                    for group in match.groups():
                        if group and not group.isspace():
                            name = group
                    
                    current_block = (i, i, "function", name)
                    continue
            
            # Check for class definition
            class_pattern = patterns.get("class")
            if class_pattern:
                match = re.match(class_pattern, line)
                if match:
                    if current_block:
                        current_block = (current_block[0], i - 1, current_block[2], current_block[3])
                        if current_block[1] >= current_block[0]:
                            boundaries.append(current_block)
                    
                    name = match.group(2) if len(match.groups()) >= 2 else ""
                    current_block = (i, i, "class", name)
        
        # Don't forget the last block
        if current_block:
            current_block = (current_block[0], len(lines), current_block[2], current_block[3])
            boundaries.append(current_block)
        
        # Merge small adjacent blocks
        return self._merge_small_chunks(boundaries, lines)
    
    def _merge_small_chunks(self, boundaries: list[tuple], lines: list[str]) -> list[tuple]:
        """Merge chunks that are too small."""
        if not boundaries:
            return []
        
        merged = []
        current = boundaries[0]
        
        for next_block in boundaries[1:]:
            current_content = "\n".join(lines[current[0]-1:current[1]])
            
            # If current chunk is small, merge with next
            if len(current_content) < self.max_chunk_size // 2:
                current = (current[0], next_block[1], "block", f"{current[3]}+{next_block[3]}")
            else:
                merged.append(current)
                current = next_block
        
        merged.append(current)
        return merged
    
    def _simple_chunk(self, content: str, file_path: str, language: str) -> list[CodeChunk]:
        """Simple line-based chunking for files without clear structure."""
        chunks = []
        lines = content.split("\n")
        imports = self._extract_imports(content, language)
        
        # Calculate lines per chunk (approximately)
        avg_line_length = len(content) / max(len(lines), 1)
        lines_per_chunk = int(self.max_chunk_size / max(avg_line_length, 1))
        overlap_lines = int(self.chunk_overlap / max(avg_line_length, 1))
        
        start = 0
        chunk_num = 0
        
        while start < len(lines):
            end = min(start + lines_per_chunk, len(lines))
            chunk_content = "\n".join(lines[start:end])
            
            chunks.append(CodeChunk(
                id=f"{file_path}:{start + 1}-{end}",
                content=chunk_content,
                file_path=file_path,
                language=language,
                start_line=start + 1,
                end_line=end,
                chunk_type="block",
                name=f"chunk_{chunk_num}",
                imports=imports if chunk_num == 0 else [],
            ))
            
            chunk_num += 1
            start = end - overlap_lines
            
            # Prevent infinite loop
            if start >= len(lines) - overlap_lines:
                break
        
        return chunks
    
    def _extract_imports(self, content: str, language: str) -> list[str]:
        """Extract import statements from code."""
        imports = []
        pattern = self.patterns.get(language, {}).get("import")
        
        if pattern:
            for line in content.split("\n")[:50]:  # Only check first 50 lines
                if re.match(pattern, line.strip()):
                    imports.append(line.strip())
        
        return imports[:20]  # Limit to 20 imports


# Convenience function
def parse_codebase(directory: str, exclude_dirs: Optional[list[str]] = None) -> Generator[CodeChunk, None, None]:
    """
    Parse all code files in a directory.
    
    Args:
        directory: Root directory to scan
        exclude_dirs: Directory names to skip
    
    Yields:
        CodeChunk objects for each chunk
    """
    parser = CodeParser()
    
    for file_path in parser.scan_directory(directory, exclude_dirs):
        for chunk in parser.parse_file(file_path):
            yield chunk
