package br.dev.ferreiras.spring_ai_llm.controller.handlers;

import br.dev.ferreiras.spring_ai_llm.dto.ExceptionDTO;
import jakarta.servlet.http.HttpServletRequest;
import org.springframework.ai.retry.NonTransientAiException;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;

import java.time.Instant;

@RestControllerAdvice
public class ControllerException {

  @ExceptionHandler(NonTransientAiException.class)
  public ResponseEntity<ExceptionDTO> aiException(NonTransientAiException exception, HttpServletRequest request) {

    HttpStatus status = HttpStatus.BAD_REQUEST;

    return ResponseEntity.ok().body(new ExceptionDTO(
            Instant.now(),
            status.value(),
            exception.getMessage(),
            request.getRequestURI()
        )
    );
  }

  @ExceptionHandler(IllegalStateException.class)
  public ResponseEntity<ExceptionDTO> aiException(IllegalStateException exception, HttpServletRequest request) {

    HttpStatus status = HttpStatus.NETWORK_AUTHENTICATION_REQUIRED;

    return ResponseEntity.ok().body(new ExceptionDTO(
            Instant.now(),
            status.value(),
            exception.getMessage(),
            request.getRequestURI()
        )
    );
  }

}
