import os
from dotenv import load_dotenv

load_dotenv()

ENDEE_URL = os.getenv("ENDEE_URL", "http://localhost:8080")
ENDEE_INDEX = os.getenv("ENDEE_INDEX", "animal_disease_index")
GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "")
EMBEDDING_MODEL = "sentence-transformers/all-MiniLM-L6-v2"
TOP_K = 3
