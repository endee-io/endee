package com.resumematcher.service;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.resumematcher.exception.BusinessException;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.http.*;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestTemplate;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

@Slf4j
@Service
@RequiredArgsConstructor
public class EmbeddingService {
    
    private final RestTemplate restTemplate;
    private final ObjectMapper objectMapper;
    
    @Value("${openai.api-key}")
    private String openAiApiKey;
    
    @Value("${openai.model}")
    private String model;
    
    @Value("${app.embedding.batch-size}")
    private int batchSize;
    
    private final ExecutorService executorService = Executors.newFixedThreadPool(5);
    
    /**
     * Generates embeddings for a single text
     */
    public List<Double> generateEmbedding(String text) {
        if (text == null || text.trim().isEmpty()) {
            throw new BusinessException("Text cannot be empty for embedding generation");
        }
        
        try {
            List<String> texts = List.of(text);
            List<List<Double>> embeddings = generateEmbeddings(texts);
            return embeddings.isEmpty() ? new ArrayList<>() : embeddings.get(0);
        } catch (Exception e) {
            log.error("Failed to generate embedding: {}", e.getMessage());
            throw new BusinessException("Failed to generate text embedding: " + e.getMessage());
        }
    }
    
    /**
     * Generates embeddings for multiple texts in batch
     */
    public List<List<Double>> generateEmbeddings(List<String> texts) {
        if (texts == null || texts.isEmpty()) {
            return new ArrayList<>();
        }
        
        List<List<Double>> allEmbeddings = new ArrayList<>();
        
        // Process in batches
        for (int i = 0; i < texts.size(); i += batchSize) {
            int end = Math.min(i + batchSize, texts.size());
            List<String> batch = texts.subList(i, end);
            List<List<Double>> batchEmbeddings = generateEmbeddingsBatch(batch);
            allEmbeddings.addAll(batchEmbeddings);
        }
        
        return allEmbeddings;
    }
    
    /**
     * Generates embeddings asynchronously
     */
    public CompletableFuture<List<Double>> generateEmbeddingAsync(String text) {
        return CompletableFuture.supplyAsync(() -> generateEmbedding(text), executorService);
    }
    
    private List<List<Double>> generateEmbeddingsBatch(List<String> texts) {
        try {
            String url = "https://api.openai.com/v1/embeddings";
            
            Map<String, Object> requestBody = new HashMap<>();
            requestBody.put("model", model);
            requestBody.put("input", texts);
            
            HttpHeaders headers = new HttpHeaders();
            headers.setContentType(MediaType.APPLICATION_JSON);
            headers.setBearerAuth(openAiApiKey);
            
            HttpEntity<String> entity = new HttpEntity<>(objectMapper.writeValueAsString(requestBody), headers);
            
            ResponseEntity<JsonNode> response = restTemplate.postForEntity(url, entity, JsonNode.class);
            
            if (response.getStatusCode() == HttpStatus.OK && response.getBody() != null) {
                JsonNode dataNode = response.getBody().get("data");
                List<List<Double>> embeddings = new ArrayList<>();
                
                for (JsonNode item : dataNode) {
                    JsonNode embeddingNode = item.get("embedding");
                    List<Double> embedding = new ArrayList<>();
                    for (JsonNode value : embeddingNode) {
                        embedding.add(value.asDouble());
                    }
                    embeddings.add(embedding);
                }
                
                return embeddings;
            } else {
                throw new BusinessException("OpenAI API returned error: " + response.getStatusCode());
            }
        } catch (Exception e) {
            log.error("Failed to generate embeddings batch: {}", e.getMessage());
            throw new BusinessException("Failed to generate embeddings: " + e.getMessage());
        }
    }
}