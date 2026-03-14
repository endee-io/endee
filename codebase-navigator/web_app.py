from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Optional

from flask import Flask, flash, redirect, render_template, request, url_for

from config import config
from endee_client import EndeeClient, EndeeError
from indexer import CodebaseIndexer
from search import SearchEngine


app = Flask(__name__)
app.secret_key = os.getenv("FLASK_SECRET_KEY", "dev-secret-key-change-me")


def _dashboard_context() -> dict[str, Any]:
    client = EndeeClient()
    server_ok = client.health_check()
    indices: list[dict[str, Any]] = []

    if server_ok:
        try:
            indices = client.list_indices()
        except EndeeError:
            indices = []

    return {
        "server_ok": server_ok,
        "indices": indices,
        "default_index": config.INDEX_NAME,
    }


def _to_bool(value: Optional[str]) -> bool:
    return str(value).lower() in {"1", "true", "yes", "on"}


@app.get("/")
def home():
    context = _dashboard_context()
    context.update({
        "search_results": [],
        "ask_answer": "",
        "ask_sources": [],
        "query": "",
        "question": "",
    })
    return render_template("index.html", **context)


@app.post("/index")
def run_indexing():
    directory = request.form.get("directory", "").strip()
    index_name = request.form.get("index_name", "").strip() or config.INDEX_NAME
    recreate = _to_bool(request.form.get("recreate", ""))
    exclude_raw = request.form.get("exclude_dirs", "").strip()

    if not directory:
        flash("Directory path is required.", "error")
        return redirect(url_for("home"))

    if not Path(directory).exists():
        flash(f"Directory not found: {directory}", "error")
        return redirect(url_for("home"))

    try:
        indexer = CodebaseIndexer(index_name=index_name)
        exclude_dirs = [item.strip() for item in exclude_raw.split(",") if item.strip()] or None
        stats = indexer.index_directory(
            directory=directory,
            exclude_dirs=exclude_dirs,
            recreate_index=recreate,
        )
        flash(
            f"Indexing finished. Indexed: {stats.get('indexed', 0)} | Errors: {stats.get('errors', 0)}",
            "success",
        )
    except Exception as exc:
        flash(f"Indexing failed: {exc}", "error")

    return redirect(url_for("home"))


@app.post("/search")
def run_search():
    context = _dashboard_context()

    query = request.form.get("query", "").strip()
    index_name = request.form.get("index_name", "").strip() or config.INDEX_NAME
    language = request.form.get("language", "").strip() or None
    path_filter = request.form.get("path_filter", "").strip() or None

    try:
        top_k = int(request.form.get("top_k", "10"))
    except ValueError:
        top_k = 10

    if not query:
        flash("Search query is required.", "error")
        context.update({
            "search_results": [],
            "ask_answer": "",
            "ask_sources": [],
            "query": "",
            "question": "",
        })
        return render_template("index.html", **context)

    try:
        engine = SearchEngine(index_name=index_name)
        results = engine.search(
            query=query,
            top_k=top_k,
            language=language,
            file_pattern=path_filter,
        )
        context.update({
            "search_results": results,
            "ask_answer": "",
            "ask_sources": [],
            "query": query,
            "question": "",
        })
    except Exception as exc:
        flash(f"Search failed: {exc}", "error")
        context.update({
            "search_results": [],
            "ask_answer": "",
            "ask_sources": [],
            "query": query,
            "question": "",
        })

    return render_template("index.html", **context)


@app.post("/ask")
def run_ask():
    context = _dashboard_context()

    question = request.form.get("question", "").strip()
    index_name = request.form.get("index_name", "").strip() or config.INDEX_NAME
    language = request.form.get("language", "").strip() or None

    try:
        top_k = int(request.form.get("top_k", "5"))
    except ValueError:
        top_k = 5

    if not question:
        flash("Question is required.", "error")
        context.update({
            "search_results": [],
            "ask_answer": "",
            "ask_sources": [],
            "query": "",
            "question": "",
        })
        return render_template("index.html", **context)

    try:
        engine = SearchEngine(index_name=index_name)
        answer = engine.ask(
            question=question,
            top_k=top_k,
            language=language,
        )
        context.update({
            "search_results": [],
            "ask_answer": answer.get("answer", ""),
            "ask_sources": answer.get("sources", []),
            "query": "",
            "question": question,
        })
    except Exception as exc:
        flash(f"Ask failed: {exc}", "error")
        context.update({
            "search_results": [],
            "ask_answer": "",
            "ask_sources": [],
            "query": "",
            "question": question,
        })

    return render_template("index.html", **context)


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000, debug=True)
