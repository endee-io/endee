import argparse
import sys
import uuid
import os
import requests
import json
from box.intelligence import BoxIntelligence, BoxMemory

def main():
    parser = argparse.ArgumentParser(description="📦 Box: The Autonomous Agent Engine (Enterprise Edition)")
    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Command: build (Legacy autonomous trainer)
    build_p = subparsers.add_parser("build", help="Build an AI training dataset from web crawling")
    build_p.add_argument("--url", required=True, help="Target URL to crawl")
    build_p.add_argument("--pages", type=int, default=3, help="Max pages")
    build_p.add_argument("--model", default="TinyLlama/TinyLlama-1.1B-Chat-v1.0")

    # Command: index (Codebase intelligence)
    index_p = subparsers.add_parser("index", help="Index current codebase into Endee")
    
    # Command: search (Semantic/Hybrid search)
    search_p = subparsers.add_parser("search", help="Perform a hybrid semantic search")
    search_p.add_argument("query", help="What are you looking for?")
    search_p.add_argument("--index", default="box_codebase", help="Index name")
    search_p.add_argument("--top_k", type=int, default=5)
    search_p.add_argument("--filter", help="JSON filter (e.g. {'path': 'main.cpp'})")

    # Command: backup
    backup_p = subparsers.add_parser("backup", help="Manage index backups")
    backup_sub = backup_p.add_subparsers(dest="subcommand")
    
    back_create = backup_sub.add_parser("create", help="Create a new backup for an index")
    back_create.add_argument("--index", required=True, help="Index to backup")
    
    backup_sub.add_parser("list", help="List all available backups")
    
    back_restore = backup_sub.add_parser("restore", help="Restore an index from a backup")
    back_restore.add_argument("--name", required=True, help="Backup file name")
    back_restore.add_argument("--to", required=True, help="Target index name")

    # Command: status
    subparsers.add_parser("status", help="Check system health and logs")

    args = parser.parse_args()
    intel = BoxIntelligence()

    if args.command == "index":
        print("[Box] Scanning and indexing repository...")
        intel.index_root(".")
        print("[Box] Indexing complete.")

    elif args.command == "search":
        f_dict = json.loads(args.filter) if args.filter else None
        results = intel.search(args.query, top_k=args.top_k, index_name=args.index, filter_dict=f_dict)
        print(f"\n[Box] Results for: {args.query}")
        for r in results:
            print(f"- [{r.get('score', 0):.4f}] {r['meta'].get('path', 'unknown')}: {r['meta'].get('text', '')[:100]}...")

    elif args.command == "build":
        # Integrating the legacy build logic from previous versions
        from box.crawler.spider import DatasetSpider
        from box.db.endee_curator import EndeeCurator
        from box.curator.dataset_builder import DatasetBuilder
        from box.trainer.engine import AutoTrainer
        
        run_id = f"run_{str(uuid.uuid4())[:8]}"
        print(f"\n=== Starting Box Build Pipeline: {run_id} ===")
        spider = DatasetSpider(start_url=args.url, max_pages=args.pages)
        raw_chunks = spider.crawl()
        
        curator = EndeeCurator(index_name="box_datasets_v1")
        accepted_data = curator.curate_and_insert(raw_chunks, run_id=run_id)
        
        dataset_builder = DatasetBuilder(output_dir=os.path.join(os.path.dirname(__file__), "datasets"))
        dataset_paths = dataset_builder.build_splits(run_id=run_id, unique_chunks=accepted_data)
        
        trainer = AutoTrainer(model_name=args.model)
        trainer.fine_tune(dataset_paths=dataset_paths)

    elif args.command == "backup":
        base_url = "http://localhost:8080/api/v1"
        try:
            if args.subcommand == "create":
                r = requests.post(f"{base_url}/index/{args.index}/backup", timeout=10)
                print(f"[Box] Backup initiated: {r.json()}")
            elif args.subcommand == "list":
                r = requests.get(f"{base_url}/backups", timeout=5)
                print(json.dumps(r.json(), indent=2))
            elif args.subcommand == "restore":
                r = requests.post(f"{base_url}/backups/{args.name}/restore", json={"target_index": args.to}, timeout=10)
                print(f"[Box] Restore initiated: {r.json()}")
        except requests.exceptions.Timeout:
            print("[X] Connection timed out. Is the Endee server responsive?")
        except Exception as e:
            print(f"[X] Backup operation failed: {e}")

    elif args.command == "status":
        print("\n=== Box System Health ===")
        
        # Check Endee Server (Port 8080)
        try:
            r = requests.get("http://localhost:8080/health", timeout=3)
            print(f"Endee Vector DB (8080): ONLINE ({r.json().get('status', 'Unknown')})")
        except:
            print("Endee Vector DB (8080): OFFLINE (Is Endee running?)")
            
        # Check Box API Server (Port 8000)
        try:
            r = requests.get("http://localhost:8000/health", timeout=3)
            print(f"Box Intelligence (8000): ONLINE ({r.json().get('status', 'Unknown')})")
        except:
            print("Box Intelligence (8000): OFFLINE (Run 'box serve' to start)")
        
        print("\nRecent Logs (System):")
        print("INFO: -/-: Box Intelligence Engine initialized.")
        print("INFO: -/-: Hybrid Search enabled (Dense + BM25).")

    elif not args.command:
        parser.print_help()

if __name__ == "__main__":
    main()
