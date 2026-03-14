import json
import os

def search_questions(skill_query, top_k=3):
    data_path = os.path.join("data", "interview_questions.json")
    
    if not os.path.exists(data_path):
        return ["Question database not found. Please run generate_big_data.py"]

    with open(data_path, "r") as f:
        questions = json.load(f)
    
    matches = []
    skill_query = skill_query.lower()

    for q in questions:
        db_skill = q["skill"].lower()
        # Fuzzy match: checks if user skill is in DB skill or vice versa
        if skill_query in db_skill or db_skill in skill_query:
            matches.append(q["question"])
    
    # Unique matches only
    matches = list(set(matches))
            
    if not matches:
        return [f"Tell me about a challenging project you did with {skill_query}."]
        
    return matches[:top_k]