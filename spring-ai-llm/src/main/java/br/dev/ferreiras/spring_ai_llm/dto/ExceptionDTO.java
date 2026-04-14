package br.dev.ferreiras.spring_ai_llm.dto;

import java.time.Instant;

public record ExceptionDTO(Instant timeStamp, Integer status, String exception, String path) {
}
