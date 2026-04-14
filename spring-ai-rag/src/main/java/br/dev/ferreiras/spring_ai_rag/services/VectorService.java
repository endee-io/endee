package br.dev.ferreiras.spring_ai_rag.services;

import br.dev.ferreiras.spring_ai_rag.model.Answer;
import br.dev.ferreiras.spring_ai_rag.model.Question;
import org.springframework.web.bind.annotation.RequestBody;

public interface VectorService {
  String getAnswer(String question);
  Answer getAnswer(Question question);
  Answer askVectorDatabase(Question question);
}
