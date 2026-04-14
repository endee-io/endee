package br.dev.ferreiras.spring_ai_rag.config;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.document.Document;
import org.springframework.ai.embedding.EmbeddingModel;
import org.springframework.ai.reader.tika.TikaDocumentReader;
import org.springframework.ai.transformer.splitter.TextSplitter;
import org.springframework.ai.transformer.splitter.TokenTextSplitter;
import org.springframework.ai.vectorstore.SimpleVectorStore;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

import java.io.File;
import java.util.List;

//@Configuration
public class VectorStoreConfig {

  private static final Logger logger = LoggerFactory.getLogger(VectorStoreConfig.class);

  @Bean
  public SimpleVectorStore simpleVectorStore(EmbeddingModel embeddingModel, VectorStoreProperties vectorStoreProperties)  {
    SimpleVectorStore store = SimpleVectorStore.builder(embeddingModel).build();
    vectorStoreProperties.setVectorStorePath("/home/rferreira/dev/spring-ai-rag/src/main/resources/vector.json");
    logger.debug("File ->: {}", vectorStoreProperties.getVectorStorePath());
    File vectorStoreFile = new File(vectorStoreProperties.getVectorStorePath());
    logger.debug("File: -> {}", vectorStoreFile);

    if (vectorStoreFile.exists()) {
      store.load(vectorStoreFile);
    } else {
      logger.debug("Loading documents into vector store");
      vectorStoreProperties.getDocumentsToLoad().forEach(document -> {
        logger.debug("Loading document: {}", document.getFilename());
        TikaDocumentReader documentReader = new TikaDocumentReader(document);
        List<Document> docs = documentReader.get();
        TextSplitter textSplitter = new TokenTextSplitter();
        List<Document> splitDocs = textSplitter.apply(docs);
        store.add(splitDocs);
      });

      store.save(vectorStoreFile);

    }

    return store;
  }
}
