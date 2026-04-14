package br.dev.ferreiras.spring_ai_rag.services;

import br.dev.ferreiras.spring_ai_rag.model.Answer;
import br.dev.ferreiras.spring_ai_rag.model.Question;
import org.springframework.ai.chat.model.ChatModel;
import org.springframework.ai.chat.model.ChatResponse;
import org.springframework.ai.chat.prompt.Prompt;
import org.springframework.ai.chat.prompt.PromptTemplate;
import org.springframework.ai.document.Document;
import org.springframework.ai.vectorstore.SearchRequest;
import org.springframework.ai.vectorstore.SimpleVectorStore;
import org.springframework.ai.vectorstore.VectorStore;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.core.io.Resource;
import org.springframework.stereotype.Service;

import java.util.List;
import java.util.Map;

@Service
public class VectorServiceImpl implements VectorService{


  private final ChatModel chatModel;
//  private final SimpleVectorStore vectorStore;
  private final VectorStore vectorStore;
  @Value("classpath:/templates/rag-prompt-template.st")
  private Resource ragPromptTemplate;

  public VectorServiceImpl(ChatModel chatModel, VectorStore vectorStore) {
    this.chatModel = chatModel;
    this.vectorStore = vectorStore;
  }

  /**
   * @param question ask a question {"question:" "What is the remainder of 757 divided by 7?}
   * @return the answer for the question above ->: 1
   */
  @Override
  public String getAnswer(String question) {
    PromptTemplate promptTemplate = new PromptTemplate(question);
    Prompt prompt = promptTemplate.create();
    ChatResponse chatResponse = chatModel.call(prompt);

    return chatResponse.getResult().getOutput().getContent();
  }

  /**
   * @param question ask a question {"question:" "What is the remainder of 757 divided by 7?}
   * @return the answer for the question above ->: 1
   */
  @Override
  public Answer getAnswer(Question question) {
    PromptTemplate promptTemplate = new PromptTemplate(question.question());
    Prompt prompt = promptTemplate.create();
    ChatResponse chatResponse = chatModel.call(prompt);

    return new Answer(chatResponse.getResult().getOutput().getContent());
  }

  /**
   * @param question ask a question to the vector database
   * @return the answer for the question above or I do not know the answer
   */
  @Override
  public Answer askVectorDatabase(Question question) {
    List<Document> documents = vectorStore.similaritySearch(
        SearchRequest
            .builder()
            .query(question.question())
            .topK(5)
            .build()
    );

    List<String> contentList = documents.stream().map(Document::getContent).toList();

    PromptTemplate promptTemplate = new PromptTemplate((ragPromptTemplate));
    Prompt prompt = promptTemplate.create(Map.of("input", question.question(), "documents", String.join("\n", contentList)));
    ChatResponse chatResponse = chatModel.call(prompt);

    return new Answer(chatResponse.getResult().getOutput().getContent());
  }
}
