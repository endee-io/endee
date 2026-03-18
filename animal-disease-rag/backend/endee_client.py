import requests
from typing import Optional, Dict, Any, List
import config

class EndeeClient:
    def __init__(self, base_url: str = config.ENDEE_URL):
        self.base_url = base_url.rstrip("/")
        self.index = config.ENDEE_INDEX
    
    def create_index(self, vector_size: int = 384, description: str = "Animal disease symptom index") -> Dict:
        url = f"{self.base_url}/indexes"
        payload = {
            "name": self.index,
            "vector_size": vector_size,
            "description": description
        }
        response = requests.post(url, json=payload)
        response.raise_for_status()
        return response.json()
    
    def index_exists(self) -> bool:
        url = f"{self.base_url}/indexes/{self.index}"
        response = requests.get(url)
        return response.status_code == 200
    
    def add_vectors(self, vectors: List[Dict[str, Any]]) -> Dict:
        url = f"{self.base_url}/indexes/{self.index}/vectors"
        payload = {"vectors": vectors}
        response = requests.post(url, json=payload)
        response.raise_for_status()
        return response.json()
    
    def search(self, vector: List[float], top_k: int = config.TOP_K, 
               filter_animal: Optional[str] = None) -> Dict:
        url = f"{self.base_url}/indexes/{self.index}/search"
        payload = {
            "vector": vector,
            "top_k": top_k
        }
        if filter_animal:
            payload["filter"] = {"animal": filter_animal}
        
        response = requests.post(url, json=payload)
        response.raise_for_status()
        return response.json()
    
    def get_index_info(self) -> Dict:
        url = f"{self.base_url}/indexes/{self.index}"
        response = requests.get(url)
        response.raise_for_status()
        return response.json()
    
    def delete_index(self) -> bool:
        url = f"{self.base_url}/indexes/{self.index}"
        response = requests.delete(url)
        return response.status_code == 200
    
    def health_check(self) -> bool:
        url = f"{self.base_url}/health"
        try:
            response = requests.get(url, timeout=5)
            return response.status_code == 200
        except requests.RequestException:
            return False
