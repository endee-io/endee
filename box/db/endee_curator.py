import endee
from sentence_transformers import SentenceTransformer
import uuid

class EndeeCurator:
    def __init__(self, index_name="box_datasets", similarity_threshold=0.90):
        self.index_name = index_name
        self.similarity_threshold = similarity_threshold
        print("[Curator] Loading Embedding Model...")
        self.model = SentenceTransformer('all-MiniLM-L6-v2')
        
        print(f"[Curator] Connecting to Endee DB (index: {self.index_name})...")
        self.client = endee.Endee()
        
        # Ensure index exists
        indexes = self.client.list_indexes()
        if not any(idx.get('name') == self.index_name for idx in indexes):
            self.client.create_index(name=self.index_name, dimension=384, space_type="cosine")
            
        self.index = self.client.get_index(self.index_name)

    def curate_and_insert(self, chunks, run_id):
        """
        Takes raw chunks, embeds them, and inserts only if unique.
        Returns the list of unique chunks accepted.
        """
        accepted = []
        rejected_count = 0
        
        for item in chunks:
            text = item["text"]
            source = item["source"]
            vector = self.model.encode(text).tolist()
            
            # Check for duplicates using Sematic Search
            try:
                results = self.index.query(vector=vector, top_k=1)
                
                # If there's a highly similar chunk, skip it
                if results and results[0].get("similarity", 0) > self.similarity_threshold:
                    rejected_count += 1
                    continue
            except Exception as e:
                # First query might fail if index is empty, safely ignore
                pass
            
            # If unique, insert and accept
            doc_id = str(uuid.uuid4())
            try:
                self.index.upsert([{
                    "id": doc_id,
                    "vector": vector,
                    "meta": {"text": text, "source": source, "run_id": run_id}
                }])
                accepted.append({"id": doc_id, "text": text, "source": source})
            except Exception as e:
                print(f"[Curator] Failed to upsert: {e}")

        print(f"[Curator] Processed {len(chunks)} chunks. Accepted: {len(accepted)}, Rejected (Duplicates): {rejected_count}")
        return accepted

    def get_all_by_run(self, run_id, total_expected=10000):
        """
        Retrieve all curated data for a specific run. Normally we'd use filters.
        Since we need raw dataset dumping, we search with a dummy zero vector or fetch via filter.
        For simplicity, Endee supports querying with a zero-vector or basic filter approach if ef allows.
        (Endee allows query with filter, we will mock a diverse query to pull diverse chunks if API doesn't support select *).
        """
        # HACK: Query with zero vector to just retrieve items based on filter
        zero_vec = [0.0] * 384
        try:
            results = self.index.query(
                vector=zero_vec, 
                top_k=total_expected, 
                filter={"run_id": {"$eq": run_id}}
            )
            return results
        except Exception as e:
            print(f"[Curator] Retrieval error: {e}")
            return []
