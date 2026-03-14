import json
import os

def generate_questions():
    skills_map = {
        "Python": ["Explain GIL in Python.", "Difference between list and tuple?", "How is memory managed in Python?", "What are decorators?", "Explain generators and yield."],
        "Java": ["What is JVM vs JRE?", "Explain Spring Boot annotations.", "Difference between Interface and Abstract class?", "How does Garbage Collection work?", "What is Multithreading?"],
        "React": ["What is Virtual DOM?", "Explain React Hooks (useEffect, useState).", "Difference between Props and State?", "What is Redux?", "Explain the Component lifecycle."],
        "Node.js": ["Explain the Event Loop.", "What is Non-blocking I/O?", "Difference between setImmediate and nextTick?", "What are Streams?", "How to handle clusters in Node?"],
        "MySQL": ["What are ACID properties?", "Difference between Inner and Left Join?", "What is Indexing?", "Explain Primary Key vs Foreign Key.", "What is normalization?"],
        "Docker": ["Container vs Virtual Machine?", "How to optimize Docker images?", "What is Docker Compose?", "Explain Docker Networking.", "What is a Dockerfile?"]
    }
    
    # Generic templates to make the database feel "infinite"
    generic_templates = [
        "What are the best practices for coding in {skill}?",
        "Explain the most common design patterns used in {skill}.",
        "How do you handle error logging and debugging in {skill}?",
        "Describe a complex problem you solved using {skill}.",
        "What are the performance bottlenecks to watch for in {skill}?"
    ]

    big_data = []
    
    # Generate structured data
    for skill, questions in skills_map.items():
        # Add specific questions
        for q in questions:
            big_data.append({"skill": skill, "question": q})
        
        # Add generic template-based questions
        for temp in generic_templates:
            big_data.append({"skill": skill, "question": temp.format(skill=skill)})

    # Save it
    os.makedirs("data", exist_ok=True)
    with open("data/interview_questions.json", "w") as f:
        json.dump(big_data, f, indent=4)
    
    print(f"✅ Generated {len(big_data)} questions in data/interview_questions.json")

if __name__ == "__main__":
    generate_questions()