package br.dev.ferreiras.spring_ai_intro.services;

import br.dev.ferreiras.spring_ai_intro.model.Answer;
import br.dev.ferreiras.spring_ai_intro.model.CapitalResponse;
import br.dev.ferreiras.spring_ai_intro.model.Question;
import br.dev.ferreiras.spring_ai_intro.model.CapitalRequest;


public interface OpenAIService {
  String getAnswer(String question);
  Answer getAnswer(Question question);
  Answer getCapital(CapitalRequest capitalRequest);
  CapitalResponse getCapitalJson(CapitalRequest capitalRequest);
}
