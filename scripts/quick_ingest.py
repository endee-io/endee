"""
Quick script to create Endee index and ingest data
Run: python scripts/quick_ingest.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from backend.ingest import get_or_create_index, ingest_data
import asyncio

async def main():
    print("=" * 80)
    print("Creating Endee Index & Ingesting Data")
    print("=" * 80)
    
    # Step 1: Create index
    print("\n[1/2] Creating/getting index...")
    try:
        index = get_or_create_index()
        print(f"✓ Index ready: {index}")
    except Exception as e:
        print(f"✗ Failed to create index: {e}")
        sys.exit(1)
    
    # Step 2: Ingest data
    print("\n[2/2] Ingesting placement data...")
    try:
        result = await ingest_data("data/placement_data.json")
        print(f"\n✓ SUCCESS!")
        print(f"  Documents: {result['documents']}")
        print(f"  Vectors: {result['ingested']}")
        print("\n🎉 Endee database is ready! Test with:")
        print("   curl -X POST http://localhost:8000/ask -H 'Content-Type: application/json' -d '{\"question\": \"TCS interview\", \"top_k\": 3}'")
    except Exception as e:
        print(f"\n✗ Failed to ingest: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())
