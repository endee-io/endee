package br.dev.ferreiras.spring_ai_llm.config;

import org.springframework.ai.anthropic.AnthropicChatModel;
import org.springframework.ai.chat.client.ChatClient;
import org.springframework.ai.openai.OpenAiChatModel;
import org.springframework.ai.vertexai.gemini.VertexAiGeminiChatModel;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration
public class ChatClientConfig {

  @Bean
  public ChatClient openAIChatClient(OpenAiChatModel chatModel) {
    return ChatClient.create(chatModel);
  }

  @Bean
  public ChatClient anthropicChatClient(AnthropicChatModel chatModel) {
    return ChatClient.create(chatModel);
  }

  @Bean
  public ChatClient vertexAiChatClient(VertexAiGeminiChatModel chatModel) {
    return ChatClient.create(chatModel);
  }
}
