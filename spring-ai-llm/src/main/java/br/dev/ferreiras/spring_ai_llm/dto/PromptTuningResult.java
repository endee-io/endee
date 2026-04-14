package br.dev.ferreiras.spring_ai_llm.dto;

import com.fasterxml.jackson.annotation.JsonRawValue;

public record PromptTuningResult(@JsonRawValue String llmResponse, EvaluationResponse evalResponse, @JsonRawValue String improvementSuggestion) {
}
