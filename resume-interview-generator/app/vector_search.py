import json
import os

def search_questions(skill_query, top_k=3):
    # This is "Mock Mode" - It searches the JSON file directly
    data_path = os.path.join("data", "interview_questions.json")
    
    if not os.path.exists(data_path):
        return ["Question database not found."]

    with open(data_path, "r") as f:
        questions = json.load(f)
    
    # Simple keyword matching
    matches = []
    for q in questions:
        if skill_query.lower() in q["skill"].lower():
            matches.append(q["question"])
            
    return matches[:top_k] if matches else [f"No specific questions for {skill_query}"]