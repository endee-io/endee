## _Roadmap to Spring Certification_ <br />

Continuing to improve my dev skills, now playing with Spring-AI and OpenAPI to be ready for the challenge of developing
an application that will check a LLM to provide a specific grade to a transaction based on the information collected 
from the customer

## _Table of contents_

- [_Overview_](#overview)
- [_Requirements_](#requirements)
- [_Project Structure_](#requirements)
- [_Howto Build and Run_](#requirements)
- [_Screenshot_](#screenshot)
- [_Links_](...)
- [_Built with_](#built-with)
- [_Code Snippet_](#requirements)
- [_Continued development_](#continued-development)
- [_Useful resources_](#useful-resources)
- [_Author_](#requirements)
- [_Portfolio_](#requirements)

## _Overview_

"Spring AI is an application framework for AI engineering. Its goal is to apply to the AI domain Spring 
ecosystem design principles such as portability and modular design and 
promote using POJOs as the building blocks of an application to the AI domain.
At its core, Spring AI addresses the fundamental challenge of AI integration: 
Connecting your enterprise Data and APIs with the AI Models." https://spring.io/projects/spring-ai

<br />

## _Requirements_

- Spring Boot project setup(https://start.spring.io)
  - Spring WEB, Spring AI dependencies
- OpenAI API Key
<hr />

## _Project Structure_

- src
    - main
    - java
        - br.dev.ferreiras.spring-ai
            - config
            - controller
                - handlers
            - dto
            - entity
            - enums
            - repository
            - services
                - exceptions
    - resources
    - test
-

## _Howto Build and Run_

  ```
  - profile active: dev
  - service socket: 127.0.0.1:8080
  - tweak a few knobs to get it up and running
  
```

## _Screenshot_

[![](./ai.webp)]()

## _Links_

- Live Site URL: <a href=https://spring.io/projects/spring-ai" target="_blank">@spring-ai</a>

## _Built with_

[![My Skills](https://skillicons.dev/icons?i=java,spring,maven,redhat,idea,git,github,)](https://skillicons.dev)

## _Code Snippet_

```java
@Service
public class OpenAIServiceImpl  implements OpenAIService{
  /**
   * @param question - ask a question to openai
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

    return parser.convert(response.getResult().getOutput().getContent());
  }
}

``` 

## _Continued development_

- Unit Tests

### _Useful resources_

- [https://spring.io] Awesome Java framework!.
- [https://start.spring.io/]  Handy startup tool.
- [https://platform.openai.com/docs/guides/text-generation]  Tools to help tackle the beast

## _Author_

<a href="mailto:ricardo@ferreiras.dev.br">Ricardo Ferreira</a>

## - _Portfolio_

<a href="https://www.ferreiras.dev.br" target="_blank">My Portfolio...</a>

