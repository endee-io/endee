package guru.springframework.springaifunctions.services;


import guru.springframework.springaifunctions.functions.WeatherServiceFunction;
import guru.springframework.springaifunctions.model.Answer;
import guru.springframework.springaifunctions.model.Question;
import guru.springframework.springaifunctions.model.WeatherRequest;
import guru.springframework.springaifunctions.model.WeatherResponse;
import lombok.NoArgsConstructor;
import lombok.RequiredArgsConstructor;
import org.springframework.ai.chat.messages.Message;
import org.springframework.ai.chat.model.ChatModel;
import org.springframework.ai.chat.prompt.Prompt;
import org.springframework.ai.chat.prompt.PromptTemplate;
import org.springframework.ai.model.ModelOptionsUtils;
import org.springframework.ai.model.function.FunctionCallback;
import org.springframework.ai.openai.OpenAiChatModel;
import org.springframework.ai.openai.OpenAiChatOptions;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.List;

/**
 * Created by jt, Spring Framework Guru.
 */
@RequiredArgsConstructor
@NoArgsConstructor(force = true)
@Service
public class OpenAIServiceImpl implements OpenAIService {

  @Value("${sfg.aiapp.apiNinjasKey}")
  private final String apiNinjasKey;

  private OpenAiChatModel openAiChatModel;

  @Override
  public Answer getAnswer(Question question) {

    var promptOptions = OpenAiChatOptions.builder()
        .functionCallbacks(List.of(FunctionCallback.builder()
            .function("CurrentWeather", new WeatherServiceFunction(apiNinjasKey))
            .description("Get the current weather for a location")
                .responseConverter((response -> {
                  String schema = ModelOptionsUtils.getJsonSchema(WeatherResponse.class, false);
                  String json = ModelOptionsUtils.toJsonString(response);
                  return schema + "\n" + json;
                }))
            .inputType(Question.class)
            .build()))
        .build();

    Message userMessage = new PromptTemplate(question.question()).createMessage();

    assert openAiChatModel != null;
    var response = openAiChatModel.call(new Prompt(List.of(userMessage), promptOptions));

    return new Answer(response.getResult().getOutput().getContent());
  }
}
