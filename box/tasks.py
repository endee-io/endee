import os
import uuid
import json
from datetime import datetime
from typing import List, Dict, Optional
from box.intelligence import BoxIntelligence

class TaskManager:
    """
    Manages autonomous developer tasks using Endee as a persistent memory store.
    Provides goal tracking and historical outcome analysis.
    """
    def __init__(self, index_name="box_tasks"):
        self.intel = BoxIntelligence(index_name=index_name)
        self.index = self.intel.get_index()

    def create_task(self, goal: str, context: Optional[str] = None) -> str:
        task_id = str(uuid.uuid4())
        timestamp = datetime.now().isoformat()
        
        task_data = {
            "id": task_id,
            "goal": goal,
            "context": context or "",
            "status": "pending",
            "created_at": timestamp,
            "updates": []
        }
        
        # Store in Endee for semantic retrieval of similar past tasks
        vector = self.intel.model.encode(goal).tolist()
        self.index.upsert([{
            "id": task_id,
            "vector": vector,
            "meta": {
                "text": goal,
                "type": "task",
                "status": "pending",
                "data": json.dumps(task_data)
            }
        }])
        return task_id

    def update_task(self, task_id: str, status: str, update_msg: str):
        # In a real scenario, we'd fetch-modify-upsert. 
        # For our MVP, we'll store the pulse in the metadata.
        timestamp = datetime.now().isoformat()
        self.index.upsert([{
            "id": task_id,
            "meta_update": {
                "status": status,
                "last_update": f"{timestamp}: {update_msg}"
            }
        }], partial_update=True)

    def list_tasks(self, status: Optional[str] = None) -> List[Dict]:
        # Perform a basic query to get all task docs
        results = self.intel.client.get_index(self.index.name).query(
            vector=[0.0]*384, # Dummy vector for broad list
            top_k=50
        )
        tasks = []
        for r in results:
            if r["meta"].get("type") == "task":
                data = json.loads(r["meta"].get("data", "{}"))
                data["current_status"] = r["meta"].get("status")
                tasks.append(data)
        return tasks

    def find_similar_tasks(self, goal: str, top_k=3) -> List[Dict]:
        """Recollect past tasks to help the agent plan."""
        return self.intel.search(goal, top_k=top_k, index_name=self.index.name)
