from pydantic import BaseModel
from typing import Optional

class AddRequest(BaseModel):
    text: str
    employee_id: str
    department: str

class SearchRequest(BaseModel):
    query: str
    department: Optional[str] = None
    top_k: int = 5