import streamlit as st
import requests

st.set_page_config(page_title="Endee Resume AI", page_icon="🤖")

st.title("📄 Resume Interview Question Generator")
st.write("Upload your resume and let Endee find relevant technical questions for you.")

uploaded_file = st.file_uploader("Upload Resume (PDF)", type="pdf")

if uploaded_file is not None:
    with st.spinner("Analyzing resume..."):
        # Send to FastAPI
        files = {"file": uploaded_file.getvalue()}
        response = requests.post("http://localhost:8000/generate-from-resume", files=files)
        
        if response.status_code == 200:
            data = response.json()
            st.success("Analysis Complete!")
            
            st.subheader("Skills Detected")
            st.write(", ".join(data["detected_skills"]))
            
            st.subheader("Recommended Interview Questions")
            for skill, questions in data["interview_prep"].items():
                with st.expander(f"Skill: {skill}"):
                    for q in questions:
                        st.write(f"❓ {q}")
        else:
            st.error("Error processing resume.")