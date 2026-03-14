# 🧭 AI Codebase Navigator

An AI-powered tool to navigate, search, and understand codebases using natural language. Built with [Endee](https://github.com/endee-io/endee) vector database.

## Features

- 🔍 **Semantic Search** - Find code using natural language queries
- 💬 **Code Q&A** - Ask questions and get AI-generated answers (RAG)
- 🔄 **Find Similar Code** - Discover similar code patterns
- 📖 **Code Explanation** - Get AI explanations for any code section
- 🏷️ **Filtered Search** - Filter by language, file path, or code type
- 🌐 **Web Dashboard** - Browser UI for status, indexing, search, and Q&A

### Running Without OpenAI Key

- If `OPENAI_API_KEY` is not set, the app uses a local deterministic embedding fallback.
- This supports **indexing + semantic search** without external APIs.
- **Ask (RAG)** and **Explain** still require `OPENAI_API_KEY`.

## Prerequisites

1. **Endee Server** - Running instance of Endee vector database
2. **OpenAI API Key** - Optional for embeddings, required for LLM responses (Ask/Explain)
3. **Python 3.10+** - With pip package manager

## Quick Start

### 1. Install Dependencies

```bash
cd codebase-navigator
pip install -r requirements.txt
```

### 2. Configure Environment

```bash
# Copy the example config
cp .env.example .env

# Edit .env with your settings:
# - ENDEE_URL: URL of your Endee server (default: http://localhost:8080)
# - ENDEE_API_KEY: Your Endee API key (if auth enabled)
# - OPENAI_API_KEY: Optional (required only for Ask/Explain)
```

### 3. Start Endee Server

Make sure Endee is running. If you have it cloned locally:

```bash
cd ../endee
./run.sh  # Linux/Mac
# or
.\run.bat  # Windows (if available)
# or build and run with CMake
```

### 4. Index Your Codebase

```bash
# Index the Endee codebase itself as an example
python cli.py index ../endee --name endee-codebase

# Or index any project
python cli.py index /path/to/your/project --name my-project
```

### 5. Search and Explore

```bash
# Semantic search
python cli.py search "authentication middleware"

# Ask questions (RAG)
python cli.py ask "how does the HNSW algorithm work?"

# Find similar code
python cli.py similar src/auth.py --start 10 --end 30

# Explain code
python cli.py explain ../endee/src/main.cpp --start 1 --end 50
```

### 6. Run the Web Frontend

```bash
python web_app.py
```

Then open `http://127.0.0.1:5000` in your browser.

## CLI Commands

### Indexing

```bash
# Index a codebase
python cli.py index <directory> [options]

Options:
  --name, -n      Index name (default: "codebase")
  --recreate, -r  Delete and recreate index
  --exclude, -e   Comma-separated directories to exclude

# Examples:
python cli.py index ./my-project --name my-project
python cli.py index ./backend --exclude "node_modules,dist,coverage"
python cli.py index ./project --recreate  # Re-index from scratch
```

### Searching

```bash
# Semantic search
python cli.py search <query> [options]

Options:
  --top-k, -k     Number of results (default: 10)
  --language, -l  Filter by programming language
  --path, -p      Filter by file path pattern
  --index, -i     Index name to search

# Examples:
python cli.py search "database connection pool"
python cli.py search "error handling" --language python
python cli.py search "api routes" --path "src/server"
```

### Q&A (RAG)

```bash
# Ask questions about the codebase
python cli.py ask <question> [options]

Options:
  --top-k, -k     Context chunks to retrieve (default: 5)
  --sources, -s   Show source file locations
  --language, -l  Filter by language

# Examples:
python cli.py ask "how does user authentication work?"
python cli.py ask "what is the purpose of the WAL module?" --sources
```

### Find Similar Code

```bash
# Find code similar to a snippet
python cli.py similar <file> [options]

Options:
  --start, -s  Start line number
  --end, -e    End line number
  --top-k, -k  Number of results

# Example:
python cli.py similar src/handlers/auth.py --start 20 --end 50
```

### Explain Code

```bash
# Get AI explanation for code
python cli.py explain <file> [options]

Options:
  --start, -s  Start line
  --end, -e    End line

# Example:
python cli.py explain src/core/hnsw.py --start 100 --end 200
```

### Utilities

```bash
# Check server status
python cli.py status

# View code with syntax highlighting
python cli.py show src/main.cpp --start 1 --end 50

# Delete an index
python cli.py delete-index my-index --force
```

## Web UI

The web dashboard includes:
- **Server Status** panel (Endee connectivity + indices)
- **Index Codebase** form (directory, index name, exclude dirs, recreate)
- **Semantic Search** form with result table
- **Ask (RAG)** form with generated answer + sources

Run it with:

```bash
python web_app.py
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                          CLI (cli.py)                           │
│                  User commands and interaction                  │
└─────────────────────────────────────────────────────────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        ▼                       ▼                       ▼
┌───────────────┐      ┌───────────────┐      ┌───────────────────┐
│   Indexer     │      │ Search Engine │      │    Embeddings     │
│ (indexer.py)  │      │ (search.py)   │      │ (embeddings.py)   │
└───────────────┘      └───────────────┘      └───────────────────┘
        │                       │                       │
        │                       │                       │
        ▼                       ▼                       ▼
┌───────────────┐      ┌───────────────┐      ┌───────────────────┐
│ Code Parser   │      │ Endee Client  │      │    OpenAI API     │
│ (parser.py)   │      │(endee_client) │      │                   │
└───────────────┘      └───────────────┘      └───────────────────┘
                                │
                                ▼
                       ┌───────────────┐
                       │ Endee Server  │
                       │(Vector Store) │
                       └───────────────┘
```

## Project Structure

```
codebase-navigator/
├── cli.py              # Command-line interface
├── config.py           # Configuration management
├── parser.py           # Code parsing and chunking
├── embeddings.py       # Embedding generation (OpenAI)
├── endee_client.py     # Endee REST API client
├── indexer.py          # Indexing pipeline
├── search.py           # Search and RAG engine
├── requirements.txt    # Python dependencies
├── .env.example        # Example environment config
└── README.md           # This file
```

## Configuration

| Variable | Description | Default |
|----------|-------------|---------|
| `ENDEE_URL` | Endee server URL | `http://localhost:8080` |
| `ENDEE_API_KEY` | API key for Endee auth | (empty) |
| `OPENAI_API_KEY` | OpenAI API key | (required) |
| `EMBEDDING_MODEL` | OpenAI embedding model | `text-embedding-3-small` |
| `INDEX_NAME` | Default index name | `codebase` |

## Supported Languages

- Python (`.py`)
- JavaScript (`.js`, `.jsx`)
- TypeScript (`.ts`, `.tsx`)
- C/C++ (`.c`, `.cpp`, `.h`, `.hpp`)
- Java (`.java`)
- Go (`.go`)
- Rust (`.rs`)
- Ruby (`.rb`)
- PHP (`.php`)
- C# (`.cs`)
- Markdown (`.md`)

## Cost Estimation

The tool uses OpenAI for:
1. **Embeddings** - `text-embedding-3-small` (~$0.02 per 1M tokens)
2. **LLM responses** - `gpt-4o-mini` for Q&A and explanations

A typical codebase with 10,000 lines costs approximately $0.01-0.05 to index.

## Tips for Best Results

1. **Be specific** in your queries: "user authentication in login handler" vs "auth"
2. **Use filters** to narrow search: `--language python --path "src/api"`
3. **Adjust top_k** for more/fewer results
4. **Re-index** (`--recreate`) after significant code changes

## Troubleshooting

### "Cannot connect to Endee server"
- Ensure Endee is running: `python cli.py status`
- Check `ENDEE_URL` in your `.env` file

### "Missing OPENAI_API_KEY"
- Set your OpenAI API key in `.env`
- Get one at https://platform.openai.com/api-keys

### "No results found"
- Make sure the codebase is indexed: `python cli.py status`
- Try broader search terms
- Check language/path filters aren't too restrictive

## License

MIT License - Feel free to use and modify for your projects!
