package br.dev.ferreiras.spring_ai_llm.dto;

import java.util.List;

public record PromptTuningRequest(String userPrompt, String systemPrompt, List<String> evaluationCriteria) {
}
