package br.dev.ferreiras.spring_ai_llm.dto;

import java.util.List;

public record EvaluationRequest(String input, String actual_output,
                                List<String> retrieval_context, List<String> evaluation_criteria) {
}
