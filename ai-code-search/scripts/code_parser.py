"""
code_parser.py

This module scans a repository folder,
extracts code files, and splits them into
smaller chunks for embedding.
"""

import os


# Supported programming languages
SUPPORTED_EXTENSIONS = [
    ".py",
    ".js",
    ".ts",
    ".java",
    ".cpp",
    ".c",
    ".go"
]


def get_code_files(repo_path):
    """
    Recursively find all supported code files in a repository.
    """
    code_files = []

    for root, dirs, files in os.walk(repo_path):
        for file in files:
            for ext in SUPPORTED_EXTENSIONS:
                if file.endswith(ext):
                    code_files.append(os.path.join(root, file))

    return code_files


def read_file(file_path):
    """
    Read a code file and return its contents.
    """
    try:
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    except Exception as e:
        print(f"Error reading {file_path}: {e}")
        return ""


def chunk_code(code, chunk_size=40):
    """
    Split code into chunks of N lines.
    """
    lines = code.split("\n")
    chunks = []

    for i in range(0, len(lines), chunk_size):
        chunk = "\n".join(lines[i:i + chunk_size])
        chunks.append(chunk)

    return chunks


def parse_repository(repo_path):
    """
    Parse a repository and return all code chunks.
    """
    all_chunks = []

    code_files = get_code_files(repo_path)

    print(f"Found {len(code_files)} code files")

    for file_path in code_files:
        code = read_file(file_path)

        if not code:
            continue

        chunks = chunk_code(code)

        for chunk in chunks:
            all_chunks.append({
                "file": file_path,
                "code": chunk
            })

    print(f"Generated {len(all_chunks)} code chunks")

    return all_chunks


# Quick test
if __name__ == "__main__":
    repo_path = "data/repos/requests"

    chunks = parse_repository(repo_path)

    for chunk in chunks[:3]:
        print("\nFILE:", chunk["file"])
        print(chunk["code"][:200])