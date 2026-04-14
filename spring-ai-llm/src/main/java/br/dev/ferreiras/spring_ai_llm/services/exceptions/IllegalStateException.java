package br.dev.ferreiras.spring_ai_llm.services.exceptions;

public class IllegalStateException extends RuntimeException{
  public IllegalStateException(String message) {
    super(message);
  }
}
