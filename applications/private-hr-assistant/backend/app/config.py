from functools import lru_cache

from pydantic import field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    endee_base_url: str = "http://127.0.0.1:8080/api/v1"
    endee_auth_token: str | None = None
    endee_index_name: str = "hr_chunks"
    embedding_model: str = "sentence-transformers/all-MiniLM-L6-v2"
    embedding_dimension: int = 384

    database_path: str = "./data/app.db"

    api_bearer_token: str = "demo-hr-token"
    jwt_secret: str = "change-me-in-production"
    jwt_algorithm: str = "HS256"
    jwt_expire_minutes: int = 60 * 24

    ollama_base_url: str = "http://127.0.0.1:11434"
    ollama_model: str = "llama3.2"

    use_cloud_llm: bool = False
    openai_api_key: str | None = None
    openai_model: str = "gpt-4o-mini"
    openai_base_url: str = "https://api.openai.com/v1"

    @field_validator("use_cloud_llm", mode="before")
    @classmethod
    def _parse_cloud_llm(cls, v):
        if isinstance(v, bool):
            return v
        if v is None:
            return False
        return str(v).lower() in ("1", "true", "yes", "on")


@lru_cache
def get_settings() -> Settings:
    return Settings()
