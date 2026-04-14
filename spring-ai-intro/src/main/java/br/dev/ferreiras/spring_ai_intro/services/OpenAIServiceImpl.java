package br.dev.ferreiras.spring_ai_intro.services;

import br.dev.ferreiras.spring_ai_intro.model.Answer;
import br.dev.ferreiras.spring_ai_intro.model.CapitalRequest;
import br.dev.ferreiras.spring_ai_intro.model.CapitalResponse;
import br.dev.ferreiras.spring_ai_intro.model.Question;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.chat.model.ChatModel;
import org.springframework.ai.chat.model.ChatResponse;
import org.springframework.ai.chat.prompt.Prompt;
import org.springframework.ai.chat.prompt.PromptTemplate;
import org.springframework.ai.converter.BeanOutputConverter;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.core.io.Resource;
import org.springframework.stereotype.Service;
import java.util.Map;

@Service
public class OpenAIServiceImpl  implements OpenAIService{
  /**
   * @param question does a question!
   * @return an answer
   */

  private final ChatModel chatModel;

  private static final Logger logger = LoggerFactory.getLogger(OpenAIServiceImpl.class);


  public OpenAIServiceImpl(ChatModel chatModel, ObjectMapper objectMapper) {
    this.chatModel = chatModel;
    this.objectMapper = objectMapper;
  }

  @Override
  public String getAnswer(String question) {
    PromptTemplate promptTemplate = new PromptTemplate(question);
    Prompt prompt = promptTemplate.create();
    ChatResponse response = chatModel.call(prompt);

    return response.getResult().getOutput().getContent();
  }

  @Override
  public Answer getAnswer(Question question) {
    PromptTemplate promptTemplate = new PromptTemplate(question.question());
    Prompt prompt = promptTemplate.create();
    ChatResponse response = chatModel.call(prompt);

    return new Answer(response.getResult().getOutput().getContent());
  }

  @Value("classpath:templates/get-capital-prompt-one.st")
  private Resource capitalInfoPrompt;

  @Value("classpath:templates/get-capital-prompt-json.st")
  private Resource capitalJsonPrompt;

  private ObjectMapper objectMapper;

  @Override
  public Answer getCapital(CapitalRequest capitalRequest) {
//    PromptTemplate promptTemplate = new PromptTemplate("What is the capital of " + capitalRequest.stateOrCountry() + "?");
    PromptTemplate promptTemplate = new PromptTemplate(capitalInfoPrompt);
    Prompt prompt = promptTemplate.create(Map.of("stateOrCountry", capitalRequest.stateOrCountry()));
    ChatResponse response = chatModel.call(prompt);

    return new Answer(response.getResult().getOutput().getContent());
  }

  @Override
  public CapitalResponse getCapitalJson(CapitalRequest capitalRequest) {

    BeanOutputConverter<CapitalResponse> parser = new BeanOutputConverter<>(CapitalResponse.class);
    String format = parser.getFormat();
    PromptTemplate promptTemplate = new PromptTemplate(capitalJsonPrompt);
    Prompt prompt = promptTemplate.create(Map.of("stateOrCountry", capitalRequest.stateOrCountry(),
        "format", format));
    ChatResponse response = chatModel.call(prompt);

    logger.info("Response, {}", response.getResult().getOutput().getContent());

//    String responseString;
//
//    try {
//      JsonNode jsonNode = objectMapper.readTree(response.getResult().getOutput().getContent());
//      responseString = jsonNode.get("answer").asText();
//    } catch (JsonProcessingException e) {
//      throw new RuntimeException(e);
//    }

//     return new Answer(response.getResult().getOutput().getContent());
//    return new Answer(responseString);

    return parser.convert(response.getResult().getOutput().getContent());

  }
}
