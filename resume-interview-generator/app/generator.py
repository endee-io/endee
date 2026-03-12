from app.vector_search import search_questions

def generate_interview_set(skills):
    interview_data = {}
    for skill in skills:
        questions = search_questions(skill)
        if questions:
            interview_data[skill] = questions
    return interview_data