package br.dev.ferreiras.spring_ai_llm.services;

import br.dev.ferreiras.spring_ai_llm.dto.PromptRequest;
import org.springframework.ai.chat.client.ChatClient;
import org.springframework.ai.chat.model.ChatResponse;
import org.springframework.stereotype.Service;

@Service
public class ChatService {

  private final  ChatClientFactory chatClientFactory;

  public ChatService(ChatClientFactory chatClientFactory) {
    this.chatClientFactory = chatClientFactory;
  }

  public ChatResponse getLLMModelResponse(PromptRequest promptRequest, String model)  {

    ChatClient chatClient = chatClientFactory.getChatClient(model);

    try {
      return chatClient.prompt()
          .system(s -> s.text(promptRequest.systemPrompt()))
          .user(u -> u.text(promptRequest.userPrompt()))
          .call()
          .chatResponse();
    } catch(RuntimeException exception) {
      throw new IllegalStateException("Error communicating with external API");
    }
  }
}
