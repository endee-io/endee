from core.ingest import ingest_document

if __name__ == "__main__":
    text = "Deep learning is a subset of machine learning using neural networks."
    doc_id = ingest_document(text)
    print("Inserted:", doc_id)