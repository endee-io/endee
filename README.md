# 👁️ AI Cataract Diagnosis Assistant

[![Python](https://img.shields.io/badge/Python-3.10+-blue?style=flat-square)](https://www.python.org/) 
[![Streamlit](https://img.shields.io/badge/Streamlit-App-success?style=flat-square)](https://streamlit.io/) 
[![License](https://img.shields.io/badge/License-Apache%202.0-green?style=flat-square)](./LICENSE)
[![Endee](https://img.shields.io/badge/VectorDB-Endee-orange?style=flat-square)](https://docs.endee.io/quick-start)

> **AI-powered cataract detection assistant** combining **YOLOv8 object detection** and **Endee vector database** for semantic Q&A. Perfect for medical AI applications, patient education, and telemedicine tools.

---

## 🚀 Project Overview

The AI Cataract Diagnosis Assistant allows users to:

- Upload eye images (JPG, JPEG, PNG) for **real-time cataract detection**.
- Visualize **annotated results** with bounding boxes and confidence scores.
- Receive **medical recommendations and actionable next steps**.
- Ask **natural language questions** about cataracts using **Endee semantic search** for accurate, context-aware answers.

**Why this project matters:**

- Combines **Deep Learning + Vector Database** for AI-driven healthcare.
- **Streamlit interface** ensures accessibility and ease of use.
- Modular and extensible for **research or clinical applications**.

---

## 🏗 Features

| Feature | Description |
|---------|-------------|
| **Eye Image Diagnosis** | Detect cataract vs normal eye using YOLOv8. |
| **Annotated Results** | Bounding boxes with confidence scores for visual feedback. |
| **Medical Knowledge Retrieval** | Semantic search using Endee DB for patient queries. |
| **Suggested Next Steps** | Practical guidance for eye care. |
| **Advanced AI Q&A** | Free-text question answering powered by vector search. |

---

## 🛠 Technology Stack

- **Python 3.10+** – Core language  
- **Streamlit** – Web app UI  
- **Ultralytics YOLOv8** – Deep learning detection  
- **Pillow** – Image processing and annotation  
- **Endee Vector Database** – Semantic search backend  
- **NumPy, Scikit-learn, Sentence Transformers** – Data processing & embeddings  

---

## 📂 Project Structure

```text
AI-Cataract-Assistant/
├─ app.py                     # Main Streamlit app
├─ model_utils.py             # Model loading & inference utilities
├─ vector_db/endee_setup.py   # Endee semantic search integration
├─ requirements.txt           # Dependencies
├─ medical_data.txt           # Medical knowledge fallback
├─ temp/                      # Temporary images (uploads/annotations)
├─ README.md                  # Project documentation
└─ docs/                      # Demo images, screenshots, GIFs
# 1️⃣ Create a virtual environment
python -m venv venv

# 2️⃣ Activate the virtual environment
# Linux / Mac
source venv/bin/activate
# Windows
venv\Scripts\activate

# 3️⃣ Install project dependencies
pip install -r requirements.txt

# 4️⃣ Run the Streamlit app
streamlit run app.py
