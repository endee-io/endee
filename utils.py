def chunk_code(code: str, max_lines: int = 20):
    """
    Splits code into manageable chunks for embedding.
    """

    if not isinstance(code, str):
        return []

    lines = code.split("\n")
    chunks = []

    for i in range(0, len(lines), max_lines):
        chunk = "\n".join(lines[i:i + max_lines]).strip()
        if chunk:
            chunks.append(chunk)

    return chunks