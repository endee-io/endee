from pypdf import PdfReader


def load_pdf(file_path: str) -> str:
    reader = PdfReader(file_path)

    text = ""

    for page in reader.pages:
        content = page.extract_text()

        if content:
            text += content + "\n"

    return text