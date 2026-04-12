import os
import sys
import uuid
import re
import json
import collections
from sentence_transformers import SentenceTransformer
import endee
from typing import List, Dict, Any, Optional

class CodeSplitter:
    def __init__(self, chunk_size=1000):
        self.chunk_size = chunk_size

    def split(self, content: str, file_path: str) -> List[str]:
        extension = os.path.splitext(file_path)[1]
        if extension in ['.cpp', '.h', '.hpp', '.c']:
            blocks = re.split(r'\n(?=[a-zA-Z_][a-zA-Z0-9_:*&\s]+[a-zA-Z0-9_]+\s*\([^;]*\)\s*\{)', content)
        elif extension == '.py':
            blocks = re.split(r'\n(?=class |def )', content)
        else:
            blocks = re.split(r'\n\n+', content)

        chunks = []
        for block in blocks:
            block = block.strip()
            if not block: continue
            if len(block) > self.chunk_size * 4:
                lines = block.split('\n')
                for i in range(0, len(lines), 50):
                    chunks.append("\n".join(lines[i:i+50]))
            else:
                chunks.append(block)
        return chunks

class SparseGenerator:
    @staticmethod
    def text_to_sparse(text: str) -> Dict[int, float]:
        """
        Simple Hybrid approach: Generate sparse term-weights from text.
        In a real scenario, use a proper BM25 tokenizer/hashing.
        For now, we use a lightweight word-hash approach to represent term importance.
        """
        words = re.findall(r'\w+', text.lower())
        counts = collections.Counter(words)
        total = sum(counts.values())
        sparse = {}
        for word, count in counts.items():
            # Use a stable hash for the term_id (Endee expects uint32)
            term_id = abs(hash(word)) % (2**32)
            # tf-idf like weight (simplified)
            weight = count / total
            sparse[term_id] = weight
        return sparse

class BoxIntelligence:
    def __init__(self, index_name="box_codebase"):
        self.index_name = index_name
        self.client = endee.Endee()
        self.model = SentenceTransformer('all-MiniLM-L6-v2')
        self.splitter = CodeSplitter()
        self.manifest_path = os.path.join(os.path.dirname(__file__), "manifest.json")

    def _load_manifest(self) -> Dict[str, str]:
        if os.path.exists(self.manifest_path):
            with open(self.manifest_path, 'r') as f:
                return json.load(f)
        return {}

    def _save_manifest(self, manifest: Dict[str, str]):
        with open(self.manifest_path, 'w') as f:
            json.dump(manifest, f, indent=2)

    def get_index(self, name: Optional[str] = None):
        target_name = name or self.index_name
        indexes = self.client.list_indexes()
        if not any(idx.get('name') == target_name for idx in indexes):
            self.client.create_index(name=target_name, dimension=384, space_type="cosine")
        return self.client.get_index(target_name)

    def index_root(self, root_dir: str):
        index = self.get_index()
        exts = ('.cpp', '.h', '.hpp', '.py', '.cmake', 'CMakeLists.txt', '.md')
        manifest = self._load_manifest()
        new_manifest = {}
        
        all_payloads = []
        files_to_process = []

        for root, _, files in os.walk(root_dir):
            if any(p in root for p in ['.git', 'build', '__pycache__', 'ide']): continue
            for f in files:
                if f.endswith(exts):
                    file_path = os.path.join(root, f)
                    rel_path = os.path.relpath(file_path, root_dir)
                    mtime = str(os.path.getmtime(file_path))
                    
                    # Delta Check
                    if manifest.get(rel_path) == mtime:
                        new_manifest[rel_path] = mtime
                        continue
                        
                    files_to_process.append((file_path, rel_path, mtime))

        if not files_to_process:
            print("[Box Turbo] No changes detected. Index is up to date.")
            return

        print(f"[Box Turbo] Processing {len(files_to_process)} changed files...")

        for file_path, rel_path, mtime in files_to_process:
            try:
                with open(file_path, 'r', encoding='utf-8', errors='ignore') as f_in:
                    content = f_in.read()
                
                chunks = self.splitter.split(content, file_path)
                embeddings = self.model.encode(chunks).tolist()
                
                for chunk, emb in zip(chunks, embeddings):
                    sparse = SparseGenerator.text_to_sparse(chunk)
                    all_payloads.append({
                        "id": str(uuid.uuid4()),
                        "vector": emb,
                        "sparse_vector": sparse,
                        "meta": {"text": chunk, "path": rel_path, "type": "code"}
                    })
                
                new_manifest[rel_path] = mtime

                # Batch upsert every 100 items
                if len(all_payloads) >= 100:
                    index.upsert(all_payloads)
                    all_payloads = []
                    
            except Exception as e:
                print(f"[!] Error processing {rel_path}: {e}")
                continue

        # Final flush
        if all_payloads:
            index.upsert(all_payloads)
        
        # Merge and save manifest
        final_manifest = {**manifest, **new_manifest}
        self._save_manifest(final_manifest)

    def search(self, query: str, top_k=5, index_name=None, filter_dict=None, hybrid=True) -> List[Dict]:
        idx_name = index_name or self.index_name
        try:
            index = self.client.get_index(idx_name)
            dense_vec = self.model.encode(query).tolist()
            
            query_params = {
                "vector": dense_vec,
                "top_k": top_k,
                "filter": filter_dict
            }
            
            if hybrid:
                query_params["sparse_vector"] = SparseGenerator.text_to_sparse(query)
            
            return index.query(**query_params)
        except Exception:
            return []

class BoxMemory:
    """
    Agentic Memory Layer for long-term storage of observations and context.
    Integrated with Endee for semantic and hybrid recall.
    """
    def __init__(self, memory_name="agent_memory"):
        self.intel = BoxIntelligence(index_name=memory_name)
        self.index = self.intel.get_index()

    def remember(self, observation: str, metadata: Optional[Dict] = None):
        """Store a new observation with semantic and sparse indexing."""
        vector = self.intel.model.encode(observation).tolist()
        sparse = SparseGenerator.text_to_sparse(observation)
        doc_id = str(uuid.uuid4())
        
        meta = metadata or {}
        meta["text"] = observation
        meta["timestamp"] = str(uuid.uuid1())
        
        self.index.upsert([{
            "id": doc_id,
            "vector": vector,
            "sparse_vector": sparse,
            "meta": meta
        }])
        return doc_id

    def recall(self, prompt: str, top_k=3, filter_dict=None) -> List[str]:
        """Recollect past observations based on a prompt."""
        results = self.intel.search(prompt, top_k=top_k, index_name=self.index.name, filter_dict=filter_dict)
        return [r["meta"]["text"] for r in results]

class DeveloperAgent:
    def __init__(self, api_key=None, base_url=None):
        self.api_key = api_key
        self.base_url = base_url or "https://api.openai.com/v1"
        self.client = None
        if self.api_key or self.base_url != "https://api.openai.com/v1":
            from openai import OpenAI
            self.client = OpenAI(api_key=self.api_key or "local-no-key", base_url=self.base_url)
        self.memory = BoxMemory()

    def develop(self, instruction: str, context: str, file_content: str = "") -> str:
        if not self.client:
            return "Error: No AI Client initialized."

        # Self-correction: check memory for past similar instructions
        past_wisdom = self.memory.recall(instruction, top_k=1)
        
        prompt = f"""
        You are an Autonomous Developer Agent in the 'Box' engine.
        
        HISTORY/MEMORY CONTEXT: {past_wisdom}
        INSTRUCTION: {instruction}
        CONTEXT FROM CODEBASE: {context}
        CURRENT FILE CONTENT: {file_content}
        
        Output the code changes clearly.
        """
        
        try:
            response = self.client.chat.completions.create(
                model="gpt-4o" if "openai" in self.base_url else "local-model",
                messages=[{"role": "user", "content": prompt}],
                temperature=0.2
            )
            ans = response.choices[0].message.content
            # Store the action in memory
            self.memory.remember(f"Handled instruction: {instruction}. Outcome: Success.")
            return ans
        except Exception as e:
            return f"Agent Error: {e}"
