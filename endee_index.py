import numpy as np
from embeddings import embed_text

documents = []
vectors = []

def add_document(text):

    global documents, vectors

    documents.append(text)

    vec = embed_text([text])[0]

    vectors.append(vec)