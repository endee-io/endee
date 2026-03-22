from __future__ import annotations

from pydantic import BaseModel, Field


class LoginRequest(BaseModel):
    username: str = "demo"


class SearchRequest(BaseModel):
    query: str = Field(..., min_length=1)
    top_k: int = Field(8, ge=1, le=64)
    dept_code: str | None = None
    doc_type: str | None = None


class ChatMessage(BaseModel):
    role: str
    content: str


class ChatRequest(BaseModel):
    messages: list[ChatMessage] = Field(..., min_length=1)
    top_k: int = Field(8, ge=1, le=64)
    dept_code: str | None = None
    doc_type: str | None = None


class SimilarRecommendRequest(BaseModel):
    chunk_id: str = Field(..., min_length=1)
    top_k: int = Field(6, ge=1, le=32)
    dept_code: str | None = None
    doc_type: str | None = None


class AgentRequest(BaseModel):
    task: str = Field(..., min_length=1)
    top_k: int = Field(6, ge=1, le=32)
    dept_code: str | None = None
    doc_type: str | None = None


class HybridSearchRequest(BaseModel):
    query: str = Field(..., min_length=1)
    top_k: int = Field(8, ge=1, le=64)
    sparse_indices: list[int] | None = None
    sparse_values: list[float] | None = None
    dept_code: str | None = None
    doc_type: str | None = None
