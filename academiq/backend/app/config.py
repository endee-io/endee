from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    endee_base_url: str = "http://localhost:8080/api/v1"
    endee_auth_token: str = ""
    openai_api_key: str = ""

    class Config:
        env_file = ".env"


settings = Settings()
