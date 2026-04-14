package br.dev.ferreiras.spring_ai_rag.controllers;


import br.dev.ferreiras.spring_ai_rag.contracts.IVectorController;
import br.dev.ferreiras.spring_ai_rag.model.Answer;
import br.dev.ferreiras.spring_ai_rag.model.Question;
import br.dev.ferreiras.spring_ai_rag.services.VectorService;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;

@RestController
public class VectorController implements IVectorController {

  private final VectorService vectorService;

  public VectorController(VectorService vectorService) {
    this.vectorService = vectorService;
  }


  public Answer askQuestion(@RequestBody Question question) {

    return vectorService.getAnswer(question);
  }

  /**
   * @param question - make a question
   * @return  return answer
   */
  @Override
  public Answer askVectorDatabase(Question question) {

    return vectorService.askVectorDatabase(question);
  }
}
