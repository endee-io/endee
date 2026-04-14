package br.dev.ferreiras.spring_ai_llm.contracts;

import br.dev.ferreiras.spring_ai_llm.dto.PromptRequest;
import br.dev.ferreiras.spring_ai_llm.dto.Restaurant;
import org.springframework.web.service.annotation.HttpExchange;
import org.springframework.web.service.annotation.PostExchange;

import java.util.List;

@HttpExchange
public interface ControllerStructured {

  @PostExchange(value="/restaurants")
  public List<Restaurant> fetchRestaurants(String model, PromptRequest promptRequest);
}
