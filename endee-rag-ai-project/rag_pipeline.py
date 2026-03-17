from search import search

def generate_answer(question, context=""):

    docs = search(question)

    combined_context = context + " " + " ".join(docs)

    answer = f"Based on the document: {combined_context}"

    return answer