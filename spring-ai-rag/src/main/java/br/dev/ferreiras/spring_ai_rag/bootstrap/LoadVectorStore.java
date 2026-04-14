package br.dev.ferreiras.spring_ai_rag.bootstrap;

import br.dev.ferreiras.spring_ai_rag.config.VectorStoreProperties;
import org.apache.hadoop.mapreduce.lib.db.TextSplitter;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.document.Document;
import org.springframework.ai.reader.tika.TikaDocumentReader;
import org.springframework.ai.transformer.splitter.TokenTextSplitter;
import org.springframework.ai.vectorstore.VectorStore;
import org.springframework.boot.CommandLineRunner;
import org.springframework.stereotype.Component;

import java.util.List;
import java.util.Objects;

@Component
public class LoadVectorStore implements CommandLineRunner {
  /**
   * @param args
   * @throws Exception
   */

  private static final Logger logger = LoggerFactory.getLogger(LoadVectorStore.class);

  private final VectorStore vectorStore;

  private final VectorStoreProperties vectorStoreProperties;

  public LoadVectorStore(VectorStore vectorStore, VectorStoreProperties vectorStoreProperties) {
    this.vectorStore = vectorStore;
    this.vectorStoreProperties = vectorStoreProperties;
  }

  @Override
  public void run(String... args) throws Exception {

    logger.info("::: LOADING VECTOR STORE :::");

    if (args.length > 0) {
      logger.info("::: ARGUMENTS PASSED: {} :::", args[0]);
    }

    if(Objects.requireNonNull(vectorStore.similaritySearch("Sportsman")).isEmpty()) {
      logger.info("::: LOADING DOCUMENTS INTO VECTOR STORE :::");
      vectorStoreProperties.getDocumentsToLoad().forEach(document -> {
        logger.info("::: LOADING DOCUMENT {} :::", document.getFilename());
        TikaDocumentReader tikaDocumentReader = new TikaDocumentReader(document);
        List<Document> documents = tikaDocumentReader.get();
        TokenTextSplitter textSplitter = new TokenTextSplitter();
        List<Document> splitDocuments = textSplitter.apply(documents);
        vectorStore.add(splitDocuments);
      });
    }
    logger.info("::: VECTOR DATABASE LOADED :::");
  }
}
