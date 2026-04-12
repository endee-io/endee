"""
Benchmarks Module: Latency & Accuracy Measurements
Compares dense vs sparse vs hybrid search performance.
"""
import time
import json
import statistics
from typing import List, Dict, Any, Optional
from pathlib import Path

from config import BENCHMARK_QUERIES, DEFAULT_TOP_K, PROJECT_ROOT


class BenchmarkRunner:
    """Runs latency and accuracy benchmarks on the RAG system."""

    def __init__(self, retriever=None, rag_pipeline=None):
        self.retriever = retriever
        self.rag_pipeline = rag_pipeline
        self.results = []

    def run_latency_benchmark(self, queries: List[str] = None,
                               top_k: int = DEFAULT_TOP_K,
                               runs_per_query: int = 3) -> Dict[str, Any]:
        """Measure search latency across all three modes."""
        if not self.retriever:
            raise RuntimeError("Retriever not initialized")

        queries = queries or BENCHMARK_QUERIES
        results = {
            "dense": [], "sparse": [], "hybrid": [],
            "queries": queries, "top_k": top_k, "runs_per_query": runs_per_query,
        }

        print(f"\n📊 Running latency benchmark ({len(queries)} queries × {runs_per_query} runs)...\n")

        for query in queries:
            for mode in ["dense", "sparse", "hybrid"]:
                latencies = []
                for _ in range(runs_per_query):
                    try:
                        result = self.retriever.search(query, mode=mode, top_k=top_k)
                        latencies.append(result["latency_ms"])
                    except Exception as e:
                        print(f"  ⚠️ Error ({mode}): {e}")
                        latencies.append(-1)

                valid = [l for l in latencies if l >= 0]
                if valid:
                    results[mode].append({
                        "query": query,
                        "latencies_ms": latencies,
                        "avg_ms": statistics.mean(valid),
                        "min_ms": min(valid),
                        "max_ms": max(valid),
                        "median_ms": statistics.median(valid),
                    })

        # Compute aggregates
        summary = {}
        for mode in ["dense", "sparse", "hybrid"]:
            all_avgs = [r["avg_ms"] for r in results[mode]]
            if all_avgs:
                summary[mode] = {
                    "avg_latency_ms": round(statistics.mean(all_avgs), 2),
                    "min_latency_ms": round(min(all_avgs), 2),
                    "max_latency_ms": round(max(all_avgs), 2),
                    "median_latency_ms": round(statistics.median(all_avgs), 2),
                    "p95_latency_ms": round(sorted(all_avgs)[int(len(all_avgs) * 0.95)] if len(all_avgs) > 1 else all_avgs[0], 2),
                    "total_queries": len(results[mode]),
                }

        results["summary"] = summary
        self.results.append({"type": "latency", "data": results})

        # Print summary
        print("\n📊 Latency Summary:")
        print(f"{'Mode':<10} {'Avg (ms)':<12} {'Min (ms)':<12} {'Max (ms)':<12} {'Median (ms)':<12}")
        print("─" * 58)
        for mode in ["dense", "sparse", "hybrid"]:
            if mode in summary:
                s = summary[mode]
                print(f"{mode:<10} {s['avg_latency_ms']:<12.2f} {s['min_latency_ms']:<12.2f} "
                      f"{s['max_latency_ms']:<12.2f} {s['median_latency_ms']:<12.2f}")

        return results

    def run_accuracy_benchmark(self, query_answer_pairs: Optional[List[Dict]] = None,
                                top_k: int = DEFAULT_TOP_K) -> Dict[str, Any]:
        """
        Measure retrieval accuracy using query-answer pairs.
        Accuracy = whether the expected answer content appears in retrieved chunks.
        """
        if not self.retriever:
            raise RuntimeError("Retriever not initialized")

        # If no ground truth provided, use a relevance-based evaluation
        if not query_answer_pairs:
            return self._run_relevance_benchmark(top_k)

        results = {"dense": [], "sparse": [], "hybrid": []}

        for pair in query_answer_pairs:
            query = pair["query"]
            expected_keywords = pair.get("keywords", [])

            for mode in ["dense", "sparse", "hybrid"]:
                try:
                    search_result = self.retriever.search(query, mode=mode, top_k=top_k)
                    retrieved_text = " ".join([r["text"].lower() for r in search_result["results"]])

                    # Check how many expected keywords appear in retrieved text
                    found = sum(1 for kw in expected_keywords if kw.lower() in retrieved_text)
                    accuracy = found / len(expected_keywords) if expected_keywords else 0

                    results[mode].append({
                        "query": query,
                        "accuracy": accuracy,
                        "keywords_found": found,
                        "keywords_total": len(expected_keywords),
                        "latency_ms": search_result["latency_ms"],
                    })
                except Exception as e:
                    print(f"  ⚠️ Error ({mode}): {e}")

        # Compute summary
        summary = {}
        for mode in ["dense", "sparse", "hybrid"]:
            if results[mode]:
                accuracies = [r["accuracy"] for r in results[mode]]
                summary[mode] = {
                    "avg_accuracy": round(statistics.mean(accuracies) * 100, 2),
                    "min_accuracy": round(min(accuracies) * 100, 2),
                    "max_accuracy": round(max(accuracies) * 100, 2),
                }

        results["summary"] = summary
        self.results.append({"type": "accuracy", "data": results})

        return results

    def _run_relevance_benchmark(self, top_k: int = DEFAULT_TOP_K) -> Dict[str, Any]:
        """Evaluate retrieval relevance by checking similarity scores."""
        queries = BENCHMARK_QUERIES
        results = {"dense": [], "sparse": [], "hybrid": []}

        for query in queries:
            for mode in ["dense", "sparse", "hybrid"]:
                try:
                    search_result = self.retriever.search(query, mode=mode, top_k=top_k)
                    similarities = [r["similarity"] for r in search_result["results"]]
                    avg_sim = statistics.mean(similarities) if similarities else 0

                    results[mode].append({
                        "query": query,
                        "avg_similarity": avg_sim,
                        "top_similarity": max(similarities) if similarities else 0,
                        "results_count": len(search_result["results"]),
                    })
                except Exception as e:
                    print(f"  ⚠️ Error ({mode}): {e}")

        summary = {}
        for mode in ["dense", "sparse", "hybrid"]:
            if results[mode]:
                avg_sims = [r["avg_similarity"] for r in results[mode]]
                top_sims = [r["top_similarity"] for r in results[mode]]
                summary[mode] = {
                    "avg_similarity": round(statistics.mean(avg_sims), 4),
                    "avg_top_similarity": round(statistics.mean(top_sims), 4),
                }

        results["summary"] = summary
        self.results.append({"type": "relevance", "data": results})
        return results

    def run_rag_benchmark(self, queries: List[str] = None,
                           runs_per_query: int = 1) -> Dict[str, Any]:
        """Benchmark the full RAG pipeline (retrieval + generation)."""
        if not self.rag_pipeline:
            raise RuntimeError("RAG pipeline not initialized")

        queries = queries or BENCHMARK_QUERIES[:3]
        results = []

        print(f"\n📊 Running RAG benchmark ({len(queries)} queries)...\n")

        for query in queries:
            latencies = {"retrieval": [], "generation": [], "total": []}

            for _ in range(runs_per_query):
                try:
                    result = self.rag_pipeline.query(query, use_memory=False)
                    latencies["retrieval"].append(result["retrieval_time_ms"])
                    latencies["generation"].append(result["generation_time_ms"])
                    latencies["total"].append(result["total_time_ms"])
                except Exception as e:
                    print(f"  ⚠️ Error: {e}")

            if latencies["total"]:
                results.append({
                    "query": query,
                    "avg_retrieval_ms": round(statistics.mean(latencies["retrieval"]), 2),
                    "avg_generation_ms": round(statistics.mean(latencies["generation"]), 2),
                    "avg_total_ms": round(statistics.mean(latencies["total"]), 2),
                })

        # Summary
        if results:
            summary = {
                "avg_retrieval_ms": round(statistics.mean([r["avg_retrieval_ms"] for r in results]), 2),
                "avg_generation_ms": round(statistics.mean([r["avg_generation_ms"] for r in results]), 2),
                "avg_total_ms": round(statistics.mean([r["avg_total_ms"] for r in results]), 2),
            }
        else:
            summary = {}

        benchmark_data = {"queries": results, "summary": summary}
        self.results.append({"type": "rag", "data": benchmark_data})
        return benchmark_data

    def save_results(self, filepath: str = None):
        """Save all benchmark results to a JSON file."""
        filepath = filepath or str(PROJECT_ROOT / "benchmark_results.json")
        with open(filepath, "w") as f:
            json.dump(self.results, f, indent=2)
        print(f"\n📁 Results saved to: {filepath}")

    def get_dashboard_data(self) -> Dict[str, Any]:
        """Get formatted data for the Streamlit dashboard."""
        dashboard = {
            "latency": None,
            "accuracy": None,
            "rag": None,
        }

        for result in self.results:
            if result["type"] == "latency":
                dashboard["latency"] = result["data"].get("summary", {})
            elif result["type"] == "accuracy":
                dashboard["accuracy"] = result["data"].get("summary", {})
            elif result["type"] == "relevance":
                dashboard["accuracy"] = result["data"].get("summary", {})
            elif result["type"] == "rag":
                dashboard["rag"] = result["data"].get("summary", {})

        return dashboard


if __name__ == "__main__":
    from retriever import HybridRetriever

    retriever = HybridRetriever()
    runner = BenchmarkRunner(retriever=retriever)

    # Run latency benchmark
    latency_results = runner.run_latency_benchmark()

    # Run relevance benchmark
    relevance_results = runner._run_relevance_benchmark()

    # Save results
    runner.save_results()
