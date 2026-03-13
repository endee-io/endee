from flask import Flask, request, jsonify, render_template
from semantic_search import search
from pdf_indexer import index_pdf

app = Flask(__name__)

@app.route("/")
def home():
    return render_template("index.html")


@app.route("/upload", methods=["POST"])
def upload():

    file = request.files["file"]

    if file.filename == "":
        return jsonify({"message":"No file selected"})

    path = "uploads/" + file.filename

    file.save(path)

    from pdf_indexer import index_pdf
    index_pdf(path)

    return jsonify({"message":"PDF uploaded and indexed"})


@app.route("/ask", methods=["POST"])
def ask():

    query = request.json["query"]

    result = search(query)

    return jsonify({"answer": result})


if __name__ == "__main__":
    app.run(debug=True)