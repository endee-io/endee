package br.dev.ferreiras.spring_ai_llm.controller;

import br.dev.ferreiras.spring_ai_llm.contracts.ControllerStructured;
import br.dev.ferreiras.spring_ai_llm.dto.PromptRequest;
import br.dev.ferreiras.spring_ai_llm.dto.Restaurant;
import br.dev.ferreiras.spring_ai_llm.services.ChatClientFactory;
import org.springframework.ai.chat.client.ChatClient;
import org.springframework.core.ParameterizedTypeReference;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.List;

@RestController
@RequestMapping(value = "/api/v1")
public class StructuredChatController implements ControllerStructured {

  private final ChatClientFactory chatClientFactory;

  public StructuredChatController(ChatClientFactory chatClientFactory) {
    this.chatClientFactory = chatClientFactory;
  }

  /**
   * @param model 
   * @param promptRequest
   * @return
   */
  @Override
  public List<Restaurant> fetchRestaurants(@RequestParam(value = "model", defaultValue = "openai") String model,
                                           @RequestBody(required = false) PromptRequest promptRequest) {

    ChatClient chatClient = chatClientFactory.getChatClient(model);

    return chatClient.prompt()
        .system(s -> s.text(promptRequest.systemPrompt()))
            .user(u -> u.text(promptRequest.userPrompt()))
            .call()
            .entity(new ParameterizedTypeReference<List<Restaurant>>() {});
  }
}
