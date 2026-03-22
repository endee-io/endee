from flask import Flask, request, render_template
import pandas as pd
from sentence_transformers import SentenceTransformer
import numpy as np

app = Flask(__name__)

# Load model and data once
model = SentenceTransformer('all-MiniLM-L6-v2')
data = pd.read_csv("dataset/news.csv")
texts = data["headline"].tolist()
embeddings = model.encode(texts)

def search(query):
    query_embedding = model.encode([query])
    scores = np.dot(embeddings, query_embedding.T)
    top = np.argsort(scores.flatten())[::-1][:3]
    return [texts[i] for i in top]

@app.route("/", methods=["GET", "POST"])
def home():
    response = None

    if request.method == "POST":
        query = request.form.get("query")
        results = search(query)

        response = "Top relevant news:\n"
        for r in results:
            response += f"- {r}\n"

    return render_template("index.html", response=response)

if __name__ == "__main__":
    app.run(debug=True)