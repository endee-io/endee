package br.dev.ferreiras.spring_ai_llm.contracts;

import br.dev.ferreiras.spring_ai_llm.dto.PromptRequest;
import br.dev.ferreiras.spring_ai_llm.dto.PromptTuningRequest;
import org.springframework.http.ResponseEntity;
import org.springframework.web.service.annotation.HttpExchange;
import org.springframework.web.service.annotation.PostExchange;

import java.io.IOException;

@HttpExchange
public interface ControllerChat {

  @PostExchange("/question")
  ResponseEntity<String> fetchAnswer(String model, PromptRequest promptRequest) throws IOException;

  @PostExchange("/promptTuning")
  ResponseEntity<?> promptTuning(String model, PromptTuningRequest promptTuningRequest) throws IOException;

}
