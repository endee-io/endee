import os

FLASK_HOST = os.getenv("FLASK_HOST", "0.0.0.0")
FLASK_PORT = int(os.getenv("FLASK_PORT", 5003))
MODEL_NAME = os.getenv("MODEL_NAME", "all-MiniLM-L6-v2")
CSV_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)), "data", "dataset.csv")
