package br.dev.ferreiras.spring_ai_rag.contracts;

import br.dev.ferreiras.spring_ai_rag.model.Answer;
import br.dev.ferreiras.spring_ai_rag.model.Question;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.service.annotation.HttpExchange;
import org.springframework.web.service.annotation.PostExchange;

@HttpExchange("/rag")
public interface IVectorController {

  @PostExchange
  public Answer askQuestion(@RequestBody Question question);

  @PostExchange("/vector")
  public Answer askVectorDatabase(@RequestBody Question question);
}
