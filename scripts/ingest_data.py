"""
Data Ingestion Script
Reads JSON data, chunks text, generates embeddings, and upserts to Endee
"""

import asyncio
import sys
from pathlib import Path
from dotenv import load_dotenv

# Add parent directory to path
sys.path.append(str(Path(__file__).parent.parent))

from backend.ingest import ingest_data

load_dotenv()

async def main():
    """Main ingestion function"""
    if len(sys.argv) < 2:
        file_path = "data/placement_data.json"
        print(f"No file specified, using default: {file_path}")
    else:
        file_path = sys.argv[1]
    
    if not Path(file_path).exists():
        print(f"Error: File {file_path} not found")
        sys.exit(1)
    
    try:
        result = await ingest_data(file_path)
        print(f"\n✓ Ingestion complete: {result}")
    except Exception as e:
        print(f"Error during ingestion: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())
