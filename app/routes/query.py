from fastapi import APIRouter
from pydantic import BaseModel
from app.services.rag import retrieve, generate_answer

router = APIRouter()


class QueryRequest(BaseModel):
    query: str


@router.post("/query")
def query_data(req: QueryRequest):
    context = retrieve(req.query)
    answer = generate_answer(context, req.query)

    return {"answer": answer}