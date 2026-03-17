import os
from google import genai
from .db_client import query_collection

# Configure API Key (we'll set this via env var or dynamically)
client = genai.Client()

def route_and_execute(user_message: str) -> str:
    """
    A simple ReAct/Router agent.
    Decides whether the user is asking for:
    1. A search/question about knowledge base (RAG)
    2. A recommendation
    3. General chat
    """
    
    routing_prompt = f"""
    You are an intelligent router. Analyze the user's message and determine the intent.
    Respond ONLY with one of the following exact words:
    RAG - if the user is asking a factual question that might be in a document/knowledge base.
    RECOMMEND - if the user is asking for a suggestion, recommendation, or similar items.
    CHAT - if it's a general greeting or conversation.
    
    User Message: "{user_message}"
    """
    
    intent_response = client.models.generate_content(
        model='gemini-2.5-flash',
        contents=routing_prompt
    ).text.strip().upper()
    
    if "RAG" in intent_response:
        # 1. Retrieve Context
        results = query_collection("knowledge_base", user_message, top_k=3)
        context = ""
        if results and results.get("documents") and results["documents"][0]:
            context = "\n".join(results["documents"][0])
            
        # 2. Generate Answer
        rag_prompt = f"""
        You are an AI Assistant answering questions based on the provided context.
        If the answer is not in the context, politely say you don't know based on the documents.
        
        Context:
        {context}
        
        Question: {user_message}
        """
        answer = client.models.generate_content(
            model='gemini-2.5-flash',
            contents=rag_prompt
        ).text
        return f"**(RAG Search)**\n\n{answer}"
        
    elif "RECOMMEND" in intent_response:
         # 1. Retrieve recommendations based on semantic similarity
        results = query_collection("recommendations", user_message, top_k=3)
        context = ""
        if results and results.get("documents") and results["documents"][0]:
             for doc, meta in zip(results["documents"][0], results["metadatas"][0]):
                 title = meta.get('title', 'Unknown Item') if meta else 'Unknown Item'
                 context += f"- **{title}**: {doc}\n"
                 
        rec_prompt = f"""
        You are an AI Recommendation Engine. The user asked for a recommendation.
        Below are the top matches from our database. Present them nicely to the user.
        
        Matches:
        {context}
        
        User Request: {user_message}
        """
        answer = client.models.generate_content(
            model='gemini-2.5-flash',
            contents=rec_prompt
        ).text
        return f"**(Recommendation Engine)**\n\n{answer}"
        
    else: # CHAT
        chat_prompt = f"""
        You are a helpful AI Assistant. Respond to the user's message kindly.
        User: {user_message}
        """
        return client.models.generate_content(
            model='gemini-2.5-flash',
            contents=chat_prompt
        ).text
