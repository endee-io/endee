package br.dev.ferreiras.spring_ai_llm.services;


import org.springframework.ai.chat.client.ChatClient;
import org.springframework.beans.factory.annotation.Qualifier;
import org.springframework.stereotype.Service;

@Service
public class ChatClientFactory {

  private final ChatClient openAIChatClient;
  private final ChatClient anthropicChatClient;
  private final ChatClient vertexAiChatClient;

  public ChatClientFactory(
      @Qualifier("openAIChatClient") ChatClient openAIChatClient,
      @Qualifier("anthropicChatClient") ChatClient anthropicChatClient,
      @Qualifier("vertexAiChatClient") ChatClient vertexAiChatClient) {
    this.openAIChatClient = openAIChatClient;
    this.anthropicChatClient = anthropicChatClient;
    this.vertexAiChatClient = vertexAiChatClient;
  }

  public ChatClient getChatClient(String modelType) {

    if ("openai".equalsIgnoreCase(modelType)) {
      return openAIChatClient;
    } else if ("anthropic".equalsIgnoreCase(modelType)) {
      return anthropicChatClient;
    } else if ("vertexai".equalsIgnoreCase(modelType)) {
      return vertexAiChatClient;
    } else {
      throw new IllegalStateException("Invalid model type: " + modelType);
    }
//    return switch (modelType) {
//      case "openai" -> {
//        yield openAIChatClient;
//      }
//
//      case "anthropic" -> {
//        yield anthropicChatClient;
//      }
//
//      case "gemini" -> {
//        yield vertexAiChatClient;
//      }
//
//      default -> {
//        throw new IllegalArgumentException("Invalid model type: " + modelType);
//      }
//
//    };
  }

}
