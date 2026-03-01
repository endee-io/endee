import requests

BASE_URL = "http://localhost:8080"

def create_index(index_name="research_index", dimension=384):
    url = f"{BASE_URL}/api/v1/index"

    payload = {
        "index_name": index_name,
        "dimension": dimension,
        "space_type": "cosine",
        "precision": "float16"
    }

    response = requests.post(url, json=payload)

    print("Status Code:", response.status_code)
    print("Response:", response.text)


if __name__ == "__main__":
    create_index()