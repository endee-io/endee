import os
import uuid
import streamlit as st
from PIL import Image
from model_utils import load_model, run_inference, draw_boxes_on_image
from vector_db.endee_setup import search  # Endee semantic search

# ============================
# Streamlit Page Settings
# ============================
st.set_page_config(
    page_title="AI Cataract Diagnosis Assistant",
    layout="centered"
)
st.title("👁️ AI Cataract Diagnosis Assistant")
st.caption("AI-powered cataract screening with advanced AI Q&A and medical knowledge retrieval")

# ============================
# Load YOLO Model
# ============================
@st.cache_resource
def get_model():
    return load_model("model/best_model.pt")

model_bundle = get_model()

# ============================
# Helper Functions
# ============================
def process_uploaded_image(uploaded_file):
    temp_dir = "temp"
    os.makedirs(temp_dir, exist_ok=True)
    temp_path = os.path.join(temp_dir, f"temp_{uuid.uuid4().hex}.png")
    with open(temp_path, "wb") as f:
        f.write(uploaded_file.getbuffer())
    return temp_path

def display_diagnosis(label, confidence):
    st.success("AI Cataract Diagnosis Report")
    st.write(f"Diagnosis: **{label.upper()}**")
    st.write(f"Confidence: **{round(confidence*100, 2)}%**")
    if label.lower() == "cataract":
        st.subheader("Possible Symptoms")
        st.write(MEDICAL_KNOWLEDGE.get("symptoms", "No data available."))
        st.subheader("Medical Recommendation")
        st.warning("Consult an ophthalmologist for professional eye examination.")
        st.subheader("Suggested Next Steps")
        st.write("""1. Schedule an eye test  
2. Avoid night driving if vision is unclear  
3. Wear UV-protective sunglasses  
4. Maintain regular eye checkups""")
    else:
        st.success("No Cataract Detected. Eye appears normal.")

# ============================
# Load Medical Knowledge from File
# ============================
def load_medical_knowledge(file_path="medical_data.txt"):
    knowledge = {}
    if not os.path.exists(file_path):
        return knowledge
    with open(file_path, "r", encoding="utf-8") as f:
        current_key = None
        current_val = []
        for line in f:
            line = line.strip()
            if not line:
                continue
            # Use headers as keys
            if line.isupper():
                if current_key:
                    knowledge[current_key.lower()] = "\n".join(current_val)
                current_key = line
                current_val = []
            else:
                current_val.append(line)
        if current_key:
            knowledge[current_key.lower()] = "\n".join(current_val)
    return knowledge

MEDICAL_KNOWLEDGE = load_medical_knowledge()

# ============================
# Get AI Answers (with fallback)
# ============================
def get_ai_answers(question):
    q_lower = question.lower()
    try:
        answers = search(question)  # Endee semantic search
        if not isinstance(answers, list):
            answers = [answers]

        if not answers or all(not a.strip() for a in answers):
            for key, ans in MEDICAL_KNOWLEDGE.items():
                if key in q_lower or any(word in q_lower for word in key.split()):
                    answers = [ans]
                    break
            else:
                answers = ["Sorry, no information available."]
        return answers
    except Exception:
        for key, ans in MEDICAL_KNOWLEDGE.items():
            if key in q_lower or any(word in q_lower for word in key.split()):
                return [ans]
        return ["Sorry, no information available."]

def ask_ai(q):
    answers = get_ai_answers(q)
    for idx, ans in enumerate(answers):
        st.markdown(f"**Answer {idx+1}:**")
        st.info(ans)
    if "conversation" not in st.session_state:
        st.session_state.conversation = []
    st.session_state.conversation.append({"question": q, "answers": answers})

# ============================
# Eye Image Upload & Inference
# ============================
uploaded_file = st.file_uploader("Upload Eye Image", type=["jpg", "jpeg", "png"])

if uploaded_file:
    temp_path = process_uploaded_image(uploaded_file)
    image = Image.open(temp_path).convert("RGB")
    st.image(image, caption="Uploaded Eye Image", width=400)

    try:
        result = run_inference(model_bundle, temp_path)
        label = result.get("label", "Unknown")
        confidence = result.get("confidence", 0.0)
        boxes = result.get("boxes", [])
        names = result.get("names", {})

        annotated_path = os.path.join("temp", f"annot_{uuid.uuid4().hex}.png")
        draw_boxes_on_image(temp_path, boxes, annotated_path, labels=names)
        st.image(annotated_path, caption="AI Detection Result", width=400)

        display_diagnosis(label, confidence)

    except Exception as e:
        st.error("Error during model prediction.")
        st.write(e)

# ============================
# Divider
# ============================
st.divider()

# ============================
# Advanced AI Q&A Section
# ============================
st.subheader("Ask AI about Cataract (Advanced Semantic Q&A)")

# Input box for free-text questions
question_input = st.text_input("Enter your question")

# Suggested related questions
related_qs = [
    "What is cataract?",
    "Symptoms of cataract",
    "Risk factors of cataract",
    "How is cataract diagnosed?",
    "Cataract treatment options",
    "How to prevent cataract?",
    "Early signs of cataract",
    "Night vision effects",
    "Lifestyle tips for cataract",
    "Cataract surgery details"
]

# Handle manual input
if question_input:
    ask_ai(question_input)

# Display related questions as buttons
st.subheader("You may also ask:")
for rq in related_qs:
    st.button(rq, key=rq, on_click=lambda q=rq: ask_ai(q))