import google.generativeai as genai
from typing import List, Dict, Any
import config

class RAGPipeline:
    def __init__(self, api_key: str = None):
        self.api_key = api_key or config.GEMINI_API_KEY
        if self.api_key:
            genai.configure(api_key=self.api_key)
            self.model = genai.GenerativeModel('gemini-1.5-flash')
    
    def build_context(self, similar_cases: List[Dict]) -> str:
        context_parts = []
        for i, case in enumerate(similar_cases, 1):
            context_parts.append(
                f"Case {i}:\n"
                f"  Animal: {case.get('animal', 'Unknown')}\n"
                f"  Symptoms: {case['symptoms']}\n"
                f"  Disease: {case['disease']}\n"
                f"  Explanation: {case['explanation']}\n"
                f"  Precautions: {case['precautions']}\n"
            )
        return "\n\n".join(context_parts)
    
    def generate_explanation(self, query: str, animal: str, similar_cases: List[Dict]) -> Dict[str, Any]:
        if not self.api_key or not self.model:
            return self._fallback_response(similar_cases)
        
        context = self.build_context(similar_cases)
        
        prompt = f"""You are a veterinary assistant. Based on the user's query and similar past cases, provide a disease prediction and explanation.

User Query:
Animal: {animal}
Symptoms: {query}

Similar Past Cases:
{context}

Please provide your response in the following JSON format:
{{
    "disease": "Predicted disease name",
    "explanation": "Detailed explanation of the disease, its causes, and why it matches the symptoms",
    "precautions": "List of basic precautions the user should take (comma-separated)",
    "confidence": "High/Medium/Low based on similarity scores"
}}

If no similar cases are found, respond with:
{{
    "disease": "Unknown",
    "explanation": "Unable to determine the disease based on the provided symptoms. Please consult a veterinarian.",
    "precautions": "Consult a veterinarian immediately",
    "confidence": "Low"
}}
"""
        
        try:
            response = self.model.generate_content(prompt)
            result_text = response.text
            
            if "```json" in result_text:
                result_text = result_text.split("```json")[1].split("```")[0]
            elif "```" in result_text:
                result_text = result_text.split("```")[1].split("```")[0]
            
            import json
            result = json.loads(result_text.strip())
            return result
            
        except Exception as e:
            print(f"Error generating explanation: {e}")
            return self._fallback_response(similar_cases)
    
    def _fallback_response(self, similar_cases: List[Dict]) -> Dict[str, Any]:
        if not similar_cases:
            return {
                "disease": "Unknown",
                "explanation": "Unable to determine the disease based on the provided symptoms. Please consult a veterinarian.",
                "precautions": "Consult a veterinarian immediately",
                "confidence": "Low"
            }
        
        top_case = similar_cases[0]
        avg_score = sum(c.get("score", 0) for c in similar_cases) / len(similar_cases)
        
        if avg_score > 0.8:
            confidence = "High"
        elif avg_score > 0.6:
            confidence = "Medium"
        else:
            confidence = "Low"
        
        return {
            "disease": top_case["disease"],
            "explanation": top_case["explanation"],
            "precautions": top_case["precautions"],
            "confidence": confidence
        }
