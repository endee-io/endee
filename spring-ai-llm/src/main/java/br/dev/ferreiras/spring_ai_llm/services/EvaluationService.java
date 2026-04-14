package br.dev.ferreiras.spring_ai_llm.services;

import br.dev.ferreiras.spring_ai_llm.dto.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.chat.client.ChatClient;
import org.springframework.ai.chat.model.ChatResponse;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestClient;
import org.springframework.web.reactive.function.client.WebClient;

import java.io.IOException;

@Service
public class EvaluationService {

  private static final Logger logger = LoggerFactory.getLogger(EvaluationService.class);

  private final ChatClientFactory chatClientFactory;

//  private final WebClient webClient;

  private final String improvementPromptMessage = """
      The following prompt did not meet the evaluation criteria:
      User Prompt: %s
      System Prompt: %s
      Evaluation Criteria: %s
      Please suggest improvements to both **user prompt** and the **system prompt** to better satisfy the evaluation criteria. Ensure the refined prompts are detailed, structured, and specific enough to guide the LLM in producing high-quality responses.
      """;

  public EvaluationService(ChatClientFactory chatClientFactory) {
    this.chatClientFactory = chatClientFactory;
  }

  public ChatResponse getLlmModelResponse(PromptRequest promptRequest, String model) throws IOException {
    ChatClient chatClient = chatClientFactory.getChatClient(model);
    return chatClient.prompt()
        .system(s -> s.text(promptRequest.systemPrompt()))
        .user(u -> u.text(promptRequest.userPrompt()))
        .call()
        .chatResponse();
  }

  public PromptTuningResult evaluateLLMResponse(PromptTuningRequest promptTuningRequest, String model) throws IOException {
    PromptRequest promptRequest = new PromptRequest(promptTuningRequest.userPrompt(), promptTuningRequest.systemPrompt());
    ChatResponse chatResp = getLlmModelResponse(promptRequest, model);
    String llmResp = chatResp.getResult().getOutput().getText();
    EvaluationResponse evaluationResult = evaluateResponse(promptTuningRequest, llmResp);

    if (Double.parseDouble(evaluationResult.score()) < 0.8) {
      PromptRequest improvementPromptRequest = new PromptRequest(
          String.format(improvementPromptMessage, promptTuningRequest.userPrompt(),
              promptTuningRequest.systemPrompt(),
              String.join("\n", promptTuningRequest.evaluationCriteria())),
          "Use prompt engineering techniques to deliver improved prompts that guide the LLM to produce high-quality and relevant results that meet the evaluation criteria. Ensure the system prompt provides clear role guidance.");
      System.out.println("improved prompt req " + improvementPromptRequest.toString());

      String improvementSuggestion = getLlmModelResponse(
          improvementPromptRequest, model)
          .getResult().getOutput().getText();
      return new PromptTuningResult(llmResp, evaluationResult, improvementSuggestion);
    }
    return new PromptTuningResult(llmResp, evaluationResult, null);
  }

  private EvaluationResponse evaluateResponse(PromptTuningRequest promptTuningRequest,
                                              String llmResp) throws IOException {
//
//    WebClient webClient = WebClient.create(this.props.getApiUrl());
//    try {
//      EvaluationRequest llmEvalReq = buildEvalRequest(promptTuningRequest, llmResp);
//      return webClient.post()
//          .uri("evaluate/")
//          .bodyValue(llmEvalReq)
//          .retrieve()
//          .bodyToMono(EvaluationResponse.class)
//          .block(); // Consider the threading model and context of your application when using block()
//    } catch (RuntimeException e) {
//      logger.error("Evaluation service error: {}", e.getMessage());
//      throw new IOException("Evaluation failed. Please try again later.", e);
//    }
//

    RestClient restClient = RestClient.create("http://127.0.0.1:8000/");
    try {
      EvaluationRequest llmEvalReq = buildEvalRequest(promptTuningRequest, llmResp);

      return restClient.post().uri("evaluate/").body(llmEvalReq).retrieve().body(EvaluationResponse.class);
    } catch (RuntimeException e) {
      logger.error("Evaluation service error: {}",  e.getMessage());
      throw new IOException("Evaluation failed. Please try again later.", e);
    }
  }

  public EvaluationRequest buildEvalRequest(PromptTuningRequest promptTuningRequest, String llmResponse) {
    return new EvaluationRequest(
        "User Prompt: "+  promptTuningRequest.userPrompt() +"\n System Prompt: " + promptTuningRequest.systemPrompt() ,
        llmResponse,
        null,
        promptTuningRequest.evaluationCriteria()
    );
  }
}
