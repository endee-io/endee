from endee_index import add_document

with open("data/documents.txt") as f:
    docs = [line.strip() for line in f.readlines()]

for d in docs:
    add_document(d)

print("Documents indexed successfully")