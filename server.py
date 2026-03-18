from flask import Flask, render_template, request, jsonify
from flask_cors import CORS
import os
import numpy as np
import pandas as pd
from dotenv import load_dotenv
import threading
from werkzeug.serving import make_server

load_dotenv()

app = Flask(__name__, template_folder='frontend')
app.config['TEMPLATES_AUTO_RELOAD'] = True
CORS(app, resources={r"/predict": {"origins": "*"}})

DATA = None
MODEL = None
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

def get_model():
    global MODEL
    if MODEL is None:
        print("Loading sentence-transformers model...")
        from sentence_transformers import SentenceTransformer
        MODEL = SentenceTransformer('all-MiniLM-L6-v2')
        print("Model loaded!")
    return MODEL

def load_data():
    global DATA
    if DATA is None:
        print("Loading dataset...")
        csv_path = os.path.join(BASE_DIR, 'data', 'dataset.csv')
        df = pd.read_csv(csv_path)
        DATA = df.to_dict('records')
        for d in DATA:
            d['search_text'] = f"{d['Animal']} {d['Symptoms']} {d['Disease']}"
        model = get_model()
        texts = [d['search_text'] for d in DATA]
        print(f"Computing embeddings for {len(DATA)} records...")
        embeddings = model.encode(texts)
        for i, d in enumerate(DATA):
            d['embedding'] = embeddings[i]
        print("Done!")
    return DATA

def find_similar(query, animal, top_k=3):
    model = get_model()
    DATA = load_data()
    search_text = f"{animal} {query}"
    query_embedding = model.encode([search_text])[0]
    best = []
    for d in DATA:
        if d['Animal'].lower() == animal.lower():
            similarity = np.dot(query_embedding, d['embedding']) / (np.linalg.norm(query_embedding) * np.linalg.norm(d['embedding']) + 1e-10)
            best.append((similarity, d))
    if not best:
        for d in DATA:
            similarity = np.dot(query_embedding, d['embedding']) / (np.linalg.norm(query_embedding) * np.linalg.norm(d['embedding']) + 1e-10)
            best.append((similarity, d))
    best.sort(reverse=True)
    return best[:top_k]

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/predict', methods=['POST'])
def predict():
    try:
        DATA = load_data()
        data = request.get_json()
        if not data:
            return jsonify({'error': 'No data provided'}), 400
        animal = data.get('animal', '')
        symptoms = data.get('symptoms', '')
        if not animal or not symptoms:
            return jsonify({'error': 'Please provide both animal and symptoms'}), 400
        best = find_similar(symptoms, animal)
        if not best:
            return jsonify({'disease': 'Unknown', 'confidence': 'Low', 'explanation': 'No matching disease found.', 'precautions': 'Consult a veterinarian.', 'top_matches': []})
        top = best[0][1]
        top_score = best[0][0]
        confidence = 'High' if top_score > 0.7 else ('Medium' if top_score > 0.5 else 'Low')
        return jsonify({
            'disease': top['Disease'],
            'confidence': confidence,
            'explanation': top['Explanation'],
            'precautions': top['Precautions'],
            'top_matches': [{'disease': b[1]['Disease'], 'symptoms': b[1]['Symptoms'], 'score': float(b[0])} for b in best]
        })
    except Exception as e:
        return jsonify({'error': str(e)}), 500

if __name__ == '__main__':
    print("="*50)
    print("AI Animal Disease Finder")
    print("Open http://localhost:5003")
    print("="*50)
    server = make_server('0.0.0.0', 5003, app, threaded=True)
    thread = threading.Thread(target=server.serve_forever)
    thread.daemon = True
    thread.start()
    try:
        import time
        while True: time.sleep(1)
    except KeyboardInterrupt:
        server.shutdown()
