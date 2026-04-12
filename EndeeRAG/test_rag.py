"""Quick test of the RAG pipeline."""
import json
from rag import RAGPipeline

pipeline = RAGPipeline()

query = "What is Endee Vector Database and what are its key features?"
print(f"\nQuery: {query}")
result = pipeline.query(query)
print(f"\nAnswer:\n{result['answer']}")
print(f"\nStats:")
print(f"  Retrieval: {result['retrieval_time_ms']:.0f}ms")
print(f"  Generation: {result['generation_time_ms']:.0f}ms")
print(f"  Total: {result['total_time_ms']:.0f}ms")
print(f"  Chunks retrieved: {result['chunks_retrieved']}")
print(f"\nCitations:")
for c in result["citations"]:
    print(f"  [Source {c['source_id']}] {c['filename']} (sim: {c['similarity']:.4f})")
