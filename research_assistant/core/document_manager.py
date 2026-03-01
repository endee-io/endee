import json
import os
import uuid

DATA_PATH = os.path.join("data", "documents.json")


def load_documents():
    if not os.path.exists(DATA_PATH):
        return []
    with open(DATA_PATH, "r") as f:
        return json.load(f)


def save_documents(docs):
    with open(DATA_PATH, "w") as f:
        json.dump(docs, f, indent=2)


def add_document(text):
    docs = load_documents()

    doc_id = f"vec_{str(uuid.uuid4())[:8]}"

    new_doc = {
        "id": doc_id,
        "text": text
    }

    docs.append(new_doc)
    save_documents(docs)

    return doc_id

def get_text_by_id(doc_id):
    docs = load_documents()
    for doc in docs:
        if doc["id"] == doc_id:
            return doc["text"]
    return None