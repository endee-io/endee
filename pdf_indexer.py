import PyPDF2
from embeddings import model
from data_store import documents, embeddings


def index_pdf(path):

    reader = PyPDF2.PdfReader(path)

    for page in reader.pages:

        text = page.extract_text()

        if text:

            # split into smaller chunks
            chunks = text.split(". ")

            for chunk in chunks:

                if len(chunk) > 20:

                    vector = model.encode(chunk)

                    documents.append(chunk)

                    embeddings.append(vector)

    print("Indexed chunks:", len(documents))