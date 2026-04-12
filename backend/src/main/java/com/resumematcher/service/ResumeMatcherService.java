package com.resumematcher.service;

import com.resumematcher.client.EndeeClient;
import com.resumematcher.dto.response.MatchResult;
import com.resumematcher.dto.response.ResumeMetadata;
import com.resumematcher.exception.BusinessException;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.stereotype.Service;
import org.springframework.web.multipart.MultipartFile;

import java.time.LocalDateTime;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;

@Slf4j
@Service
@RequiredArgsConstructor
public class ResumeMatcherService {
    
    private final EndeeClient endeeClient;
    private final EmbeddingService embeddingService;
    private final ResumeParserService parserService;
    
    // In-memory cache for resume metadata (in production, use a real database)
    private final Map<String, ResumeMetadata> resumeCache = new ConcurrentHashMap<>();
    
    /**
     * Uploads and processes a resume
     */
    public ResumeMetadata uploadResume(MultipartFile file) {
        try {
            // Extract text from resume
            String resumeText = parserService.extractText(file);
            
            // Extract metadata
            String email = parserService.extractEmail(resumeText);
            String name = parserService.extractName(resumeText);
            String[] skills = parserService.extractSkills(resumeText);
            
            // Generate embedding
            List<Double> embedding = embeddingService.generateEmbedding(resumeText);
            
            // Create unique ID
            String resumeId = UUID.randomUUID().toString();
            
            // Prepare metadata
            ResumeMetadata metadata = ResumeMetadata.builder()
                    .id(resumeId)
                    .filename(file.getOriginalFilename())
                    .candidateName(name != null ? name : "Unknown Candidate")
                    .email(email)
                    .skills(skills)
                    .uploadedAt(LocalDateTime.now())
                    .fileSize(file.getSize())
                    .build();
            
            // Store in Endee
            Map<String, Object> endeeMetadata = new HashMap<>();
            endeeMetadata.put("resumeId", resumeId);
            endeeMetadata.put("candidateName", metadata.getCandidateName());
            endeeMetadata.put("filename", metadata.getFilename());
            endeeMetadata.put("email", metadata.getEmail());
            endeeMetadata.put("skills", String.join(",", metadata.getSkills()));
            endeeMetadata.put("uploadedAt", metadata.getUploadedAt().toString());
            
            endeeClient.insertVector(resumeId, embedding, endeeMetadata);
            
            // Cache metadata
            resumeCache.put(resumeId, metadata);
            
            log.info("Successfully uploaded resume: {} (ID: {})", file.getOriginalFilename(), resumeId);
            return metadata;
            
        } catch (Exception e) {
            log.error("Failed to upload resume: {}", e.getMessage());
            throw new BusinessException("Failed to process resume upload: " + e.getMessage());
        }
    }
    
    /**
     * Matches resumes against job description
     */
    public List<MatchResult> matchResumes(String jobDescription, int topK) {
        try {
            // Generate embedding for job description
            List<Double> jobEmbedding = embeddingService.generateEmbedding(jobDescription);
            
            // Search in Endee
            List<EndeeClient.EndeeSearchResult> searchResults = endeeClient.searchSimilar(jobEmbedding, topK);
            
            // Convert to match results
            List<MatchResult> matches = new ArrayList<>();
            for (EndeeClient.EndeeSearchResult result : searchResults) {
                String resumeId = result.getId();
                double similarityScore = result.getScore() * 100; // Convert to percentage
                
                // Get metadata from cache or from result
                ResumeMetadata metadata = resumeCache.get(resumeId);
                if (metadata == null && result.getMetadata() != null) {
                    metadata = ResumeMetadata.builder()
                            .id(resumeId)
                            .candidateName((String) result.getMetadata().get("candidateName"))
                            .filename((String) result.getMetadata().get("filename"))
                            .email((String) result.getMetadata().get("email"))
                            .skills(((String) result.getMetadata().get("skills")).split(","))
                            .build();
                }
                
                MatchResult match = MatchResult.builder()
                        .resumeId(resumeId)
                        .candidateName(metadata != null ? metadata.getCandidateName() : "Unknown")
                        .filename(metadata != null ? metadata.getFilename() : "Unknown")
                        .email(metadata != null ? metadata.getEmail() : null)
                        .skills(metadata != null ? metadata.getSkills() : new String[0])
                        .similarityScore(similarityScore)
                        .matchGrade(getMatchGrade(similarityScore))
                        .build();
                
                matches.add(match);
            }
            
            // Sort by similarity score descending
            matches.sort((a, b) -> Double.compare(b.getSimilarityScore(), a.getSimilarityScore()));
            
            log.info("Found {} matches for job description", matches.size());
            return matches;
            
        } catch (Exception e) {
            log.error("Failed to match resumes: {}", e.getMessage());
            throw new BusinessException("Failed to match resumes: " + e.getMessage());
        }
    }
    
    /**
     * Gets all uploaded resumes
     */
    public List<ResumeMetadata> getAllResumes() {
        return new ArrayList<>(resumeCache.values());
    }
    
    /**
     * Gets resume by ID
     */
    public ResumeMetadata getResume(String resumeId) {
        return resumeCache.get(resumeId);
    }
    
    private String getMatchGrade(double score) {
        if (score >= 90) return "Excellent Match";
        if (score >= 75) return "Strong Match";
        if (score >= 60) return "Good Match";
        if (score >= 40) return "Potential Match";
        return "Low Match";
    }
}