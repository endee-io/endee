import os
import glob
import uuid
import re
import sys
from sentence_transformers import SentenceTransformer

# Add parent directory to path to import box components
sys.path.append(os.path.dirname(os.path.dirname(__file__)))
from box.db.endee_curator import EndeeCurator

def chunk_markdown(file_path, max_words=150):
    """Simple parser to read a markdown file and split it into chunks of paragraphs."""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Failed to read {file_path}: {e}")
        return []

    # Basic chunking by double newlines (paragraphs/blocks)
    blocks = re.split(r'\n\n+', content)
    
    chunks = []
    current_chunk = ""
    file_name = os.path.basename(file_path)

    for block in blocks:
        block = block.strip()
        if not block:
            continue
        # If the block is huge, just add it anyway.
        if len(current_chunk.split()) + len(block.split()) > max_words:
            if current_chunk:
                chunks.append({"text": current_chunk, "source": file_name})
            current_chunk = block
        else:
            current_chunk += "\n\n" + block if current_chunk else block

    if current_chunk:
        chunks.append({"text": current_chunk, "source": file_name})

    return chunks

def main():
    print("🚀 Starting Smart Ingestion Pipeline...")
    
    # 1. Gather files
    docs_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "docs")
    readme_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "README.md")
    
    files_to_parse = glob.glob(os.path.join(docs_dir, "*.md"))
    if os.path.exists(readme_path):
        files_to_parse.append(readme_path)

    print(f"Found {len(files_to_parse)} markdown files to parse.")

    all_chunks = []
    for f in files_to_parse:
        chunks = chunk_markdown(f)
        all_chunks.extend(chunks)

    print(f"Parsed into {len(all_chunks)} total chunks.")

    # 2. Use EndeeCurator for Semantic Deduplication
    # We use the same index name as in app.py to ensure the RAG assistant uses the deduplicated data
    run_id = f"ingest_{str(uuid.uuid4())[:8]}"
    curator = EndeeCurator(index_name="endee_docs", similarity_threshold=0.95)
    
    print(f"--- [Semantic Deduplication Phase] ---")
    accepted_data = curator.curate_and_insert(all_chunks, run_id=run_id)
    
    new_chunks = len(accepted_data)
    skipped = len(all_chunks) - new_chunks
    
    print("\n--- Ingestion Summary ---")
    print(f"Total Chunks Processed: {len(all_chunks)}")
    print(f"New Chunks Indexed:    {new_chunks}")
    print(f"Duplicates Skipped:    {skipped}")
    print(f"Run ID:                {run_id}")
    print("--------------------------")
    print("Data ingestion complete!")

if __name__ == "__main__":
    main()
