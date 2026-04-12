package com.resumematcher.client;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.resumematcher.config.EndeeConfig;
import com.resumematcher.exception.BusinessException;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.http.*;
import org.springframework.stereotype.Component;
import org.springframework.web.client.RestClientException;
import org.springframework.web.client.RestTemplate;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

@Slf4j
@Component
@RequiredArgsConstructor
public class EndeeClient {
    
    private final RestTemplate restTemplate;
    private final ObjectMapper objectMapper;
    private final EndeeConfig endeeConfig;
    
    /**
     * Creates an index in Endee if it doesn't already exist
     */
    public void createIndexIfNotExists() {
        try {
            // Check if index exists
            String checkUrl = endeeConfig.getBaseUrl() + "/v1/indexes/" + endeeConfig.getIndexName();
            try {
                ResponseEntity<JsonNode> response = restTemplate.getForEntity(checkUrl, JsonNode.class);
                if (response.getStatusCode() == HttpStatus.OK) {
                    log.info("Index '{}' already exists", endeeConfig.getIndexName());
                    return;
                }
            } catch (RestClientException e) {
                log.info("Index '{}' not found, creating new one", endeeConfig.getIndexName());
            }
            
            // Create index
            String createUrl = endeeConfig.getBaseUrl() + "/v1/indexes";
            ObjectNode requestBody = objectMapper.createObjectNode();
            requestBody.put("name", endeeConfig.getIndexName());
            requestBody.put("dimension", endeeConfig.getVectorDimension());
            requestBody.put("metric", endeeConfig.getSimilarityMetric());
            
            HttpHeaders headers = new HttpHeaders();
            headers.setContentType(MediaType.APPLICATION_JSON);
            HttpEntity<String> entity = new HttpEntity<>(requestBody.toString(), headers);
            
            ResponseEntity<JsonNode> response = restTemplate.postForEntity(createUrl, entity, JsonNode.class);
            
            if (response.getStatusCode() == HttpStatus.CREATED || response.getStatusCode() == HttpStatus.OK) {
                log.info("Successfully created index '{}'", endeeConfig.getIndexName());
            } else {
                log.warn("Unexpected response while creating index: {}", response.getStatusCode());
            }
        } catch (Exception e) {
            log.error("Failed to create index: {}", e.getMessage());
            throw new BusinessException("Failed to initialize vector database: " + e.getMessage());
        }
    }
    
    /**
     * Inserts a vector with metadata into Endee
     */
    public void insertVector(String id, List<Double> vector, Map<String, Object> metadata) {
        try {
            String url = endeeConfig.getBaseUrl() + "/v1/indexes/" + endeeConfig.getIndexName() + "/vectors";
            
            ObjectNode requestBody = objectMapper.createObjectNode();
            requestBody.put("id", id);
            
            ArrayNode vectorArray = objectMapper.createArrayNode();
            for (Double value : vector) {
                vectorArray.add(value);
            }
            requestBody.set("vector", vectorArray);
            
            ObjectNode metadataNode = objectMapper.valueToTree(metadata);
            requestBody.set("metadata", metadataNode);
            
            HttpHeaders headers = new HttpHeaders();
            headers.setContentType(MediaType.APPLICATION_JSON);
            HttpEntity<String> entity = new HttpEntity<>(requestBody.toString(), headers);
            
            ResponseEntity<JsonNode> response = restTemplate.postForEntity(url, entity, JsonNode.class);
            
            if (response.getStatusCode() == HttpStatus.CREATED || response.getStatusCode() == HttpStatus.OK) {
                log.debug("Successfully inserted vector with id: {}", id);
            } else {
                log.error("Failed to insert vector: {}", response.getStatusCode());
                throw new BusinessException("Failed to insert vector into Endee");
            }
        } catch (Exception e) {
            log.error("Error inserting vector: {}", e.getMessage());
            throw new BusinessException("Failed to store resume embedding: " + e.getMessage());
        }
    }
    
    /**
     * Searches for similar vectors in Endee
     */
    public List<EndeeSearchResult> searchSimilar(List<Double> queryVector, int topK) {
        try {
            String url = endeeConfig.getBaseUrl() + "/v1/indexes/" + endeeConfig.getIndexName() + "/search";
            
            ObjectNode requestBody = objectMapper.createObjectNode();
            ArrayNode vectorArray = objectMapper.createArrayNode();
            for (Double value : queryVector) {
                vectorArray.add(value);
            }
            requestBody.set("vector", vectorArray);
            requestBody.put("topK", topK);
            requestBody.put("includeMetadata", true);
            
            HttpHeaders headers = new HttpHeaders();
            headers.setContentType(MediaType.APPLICATION_JSON);
            HttpEntity<String> entity = new HttpEntity<>(requestBody.toString(), headers);
            
            ResponseEntity<JsonNode> response = restTemplate.postForEntity(url, entity, JsonNode.class);
            
            if (response.getStatusCode() == HttpStatus.OK && response.getBody() != null) {
                List<EndeeSearchResult> results = new ArrayList<>();
                JsonNode resultsNode = response.getBody().get("results");
                
                if (resultsNode != null && resultsNode.isArray()) {
                    for (JsonNode resultNode : resultsNode) {
                        EndeeSearchResult result = new EndeeSearchResult();
                        result.setId(resultNode.get("id").asText());
                        result.setScore(resultNode.get("score").asDouble());
                        
                        if (resultNode.has("metadata")) {
                            JsonNode metadata = resultNode.get("metadata");
                            Map<String, Object> metadataMap = objectMapper.convertValue(metadata, Map.class);
                            result.setMetadata(metadataMap);
                        }
                        
                        results.add(result);
                    }
                }
                
                log.debug("Found {} similar vectors", results.size());
                return results;
            } else {
                log.error("Search failed with status: {}", response.getStatusCode());
                return new ArrayList<>();
            }
        } catch (Exception e) {
            log.error("Error searching vectors: {}", e.getMessage());
            throw new BusinessException("Failed to search similar resumes: " + e.getMessage());
        }
    }
    
    /**
     * Deletes a vector from Endee
     */
    public void deleteVector(String id) {
        try {
            String url = endeeConfig.getBaseUrl() + "/v1/indexes/" + endeeConfig.getIndexName() + "/vectors/" + id;
            restTemplate.delete(url);
            log.debug("Deleted vector with id: {}", id);
        } catch (Exception e) {
            log.error("Error deleting vector: {}", e.getMessage());
        }
    }
    
    @lombok.Data
    public static class EndeeSearchResult {
        private String id;
        private double score;
        private Map<String, Object> metadata;
    }
}