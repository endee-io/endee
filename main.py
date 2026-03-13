"""
main.py — CLI entrypoint for the RAG pipeline.

Usage:
  python main.py ingest  --path ./sample_data/
  python main.py query   --question "Recommend a sci-fi movie"
  python main.py serve
"""

import argparse
import logging
import sys

# ── Logging setup ──────────────────────────────────────────────────────

def setup_logging(verbose: bool = False):
    """Configure structured logging."""
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s │ %(levelname)-8s │ %(name)-25s │ %(message)s",
        datefmt="%H:%M:%S",
    )


# ── CLI commands ───────────────────────────────────────────────────────

def cmd_ingest(args):
    """Run the ingestion pipeline."""
    from ingestion.pipeline import run_ingestion

    print(f"\n📥  Ingesting documents from: {args.path}\n")
    result = run_ingestion(args.path)
    print(f"\n✅  Ingestion complete!")
    print(f"    Documents loaded : {result['documents']}")
    print(f"    Chunks created   : {result['chunks']}")
    print(f"    Status           : {result['status']}\n")


def cmd_query(args):
    """Run a single RAG query."""
    from retrieval.retriever import retrieve, format_context
    from agent.llm_client import answer_question

    print(f"\n🔍  Question: {args.question}\n")

    # Retrieve context
    print("   Searching Endee for relevant context …")
    results = retrieve(args.question, top_k=args.top_k)
    context = format_context(results)

    if not results:
        print("   ⚠️  No relevant documents found. Have you run ingestion?")
        return

    print(f"   Found {len(results)} relevant chunk(s)\n")

    # Generate answer
    print("   Generating answer via LLM …\n")
    answer = answer_question(args.question, context)

    print("─" * 60)
    print(f"\n💬  Answer:\n\n{answer}\n")
    print("─" * 60)
    print("\n📚  Sources:")
    for i, r in enumerate(results, 1):
        fname = r["metadata"].get("filename", "unknown")
        score = r.get("score", 0)
        print(f"    [{i}] {fname}  (relevance: {score:.3f})")
    print()


def cmd_serve(args):
    """Start the FastAPI server."""
    import uvicorn
    from config.settings import settings

    print(f"\n🚀  Starting RAG Agent API on http://{settings.api_host}:{settings.api_port}\n")
    print(f"    Docs:   http://localhost:{settings.api_port}/docs")
    print(f"    Health: http://localhost:{settings.api_port}/health\n")

    uvicorn.run(
        "api.server:app",
        host=settings.api_host,
        port=settings.api_port,
        reload=args.reload,
    )


# ── Argument parser ───────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        prog="rag-agent",
        description="RAG Pipeline with Endee Vector Database",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable debug logging"
    )

    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # ── ingest ──
    p_ingest = subparsers.add_parser("ingest", help="Ingest documents into Endee")
    p_ingest.add_argument(
        "--path", required=True, help="Path to a file or directory to ingest"
    )

    # ── query ──
    p_query = subparsers.add_parser("query", help="Ask a question using RAG")
    p_query.add_argument(
        "--question", "-q", required=True, help="The question to ask"
    )
    p_query.add_argument(
        "--top-k", type=int, default=5, help="Number of chunks to retrieve (default: 5)"
    )

    # ── serve ──
    p_serve = subparsers.add_parser("serve", help="Start the API server")
    p_serve.add_argument(
        "--reload", action="store_true", help="Enable auto-reload for development"
    )

    args = parser.parse_args()
    setup_logging(args.verbose)

    if args.command is None:
        parser.print_help()
        sys.exit(1)

    commands = {
        "ingest": cmd_ingest,
        "query": cmd_query,
        "serve": cmd_serve,
    }

    try:
        commands[args.command](args)
    except KeyboardInterrupt:
        print("\n\n👋  Shutdown requested. Goodbye!\n")
    except Exception as e:
        logging.error(f"Fatal error: {e}", exc_info=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
