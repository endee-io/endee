import os
import json
import random

class DatasetBuilder:
    def __init__(self, output_dir="datasets"):
        self.output_dir = output_dir

    def build_splits(self, run_id, unique_chunks, train_pct=0.8, dev_pct=0.1):
        """
        Shuffles and segments data into structured train/dev/test folders.
        Formats data into instruction-style JSONL.
        """
        if not unique_chunks:
            print("[DatasetBuilder] No data to build.")
            return None

        # Shuffle
        random.shuffle(unique_chunks)
        
        n = len(unique_chunks)
        train_end = int(n * train_pct)
        dev_end = train_end + int(n * dev_pct)
        
        splits = {
            "train": unique_chunks[:train_end],
            "dev": unique_chunks[train_end:dev_end],
            "test": unique_chunks[dev_end:]
        }
        
        run_dir = os.path.join(self.output_dir, run_id)
        
        paths = {}
        print(f"[DatasetBuilder] Writing splits to {run_dir}...")
        for split_name, data in splits.items():
            if not data:
                continue
                
            split_dir = os.path.join(run_dir, split_name)
            os.makedirs(split_dir, exist_ok=True)
            
            filepath = os.path.join(split_dir, "data.jsonl")
            paths[split_name] = filepath
            
            with open(filepath, "w", encoding="utf-8") as f:
                for item in data:
                    # Instruct format format
                    record = {
                        "text": f"Context: {item['text']}",
                        "source": item['source']
                    }
                    f.write(json.dumps(record) + "\n")
                    
        print(f"[DatasetBuilder] Success! Created Train({len(splits['train'])}), Dev({len(splits['dev'])}), Test({len(splits['test'])})")
        return paths
