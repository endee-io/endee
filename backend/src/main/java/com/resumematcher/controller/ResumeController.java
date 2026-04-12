package com.resumematcher.controller;

import com.resumematcher.dto.request.JobDescriptionRequest;
import com.resumematcher.dto.response.MatchResult;
import com.resumematcher.dto.response.ResumeMetadata;
import com.resumematcher.service.ResumeMatcherService;
import jakarta.validation.Valid;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;
import org.springframework.web.multipart.MultipartFile;

import java.util.List;

@Slf4j
@RestController
@RequestMapping("/api")
@RequiredArgsConstructor
@CrossOrigin(origins = "http://localhost:5173")
public class ResumeController {
    
    private final ResumeMatcherService resumeMatcherService;
    
    /**
     * Upload a resume file
     */
    @PostMapping("/resumes/upload")
    public ResponseEntity<ResumeMetadata> uploadResume(
            @RequestParam("file") MultipartFile file) {
        
        log.info("Received resume upload request: {}", file.getOriginalFilename());
        
        // Validate file
        if (file.isEmpty()) {
            throw new IllegalArgumentException("File is empty");
        }
        
        // Validate file type
        String filename = file.getOriginalFilename();
        if (filename != null && !isValidFileType(filename)) {
            throw new IllegalArgumentException("Invalid file type. Only PDF, DOC, DOCX, and TXT files are allowed");
        }
        
        ResumeMetadata metadata = resumeMatcherService.uploadResume(file);
        return ResponseEntity.status(HttpStatus.CREATED).body(metadata);
    }
    
    /**
     * Match resumes against job description
     */
    @PostMapping("/match")
    public ResponseEntity<List<MatchResult>> matchResumes(
            @Valid @RequestBody JobDescriptionRequest request) {
        
        log.info("Received match request for job description (length: {} chars)", 
                request.getJobDescription().length());
        
        List<MatchResult> matches = resumeMatcherService.matchResumes(
                request.getJobDescription(), 
                request.getTopK()
        );
        
        return ResponseEntity.ok(matches);
    }
    
    /**
     * Get all uploaded resumes
     */
    @GetMapping("/resumes")
    public ResponseEntity<List<ResumeMetadata>> getAllResumes() {
        log.info("Fetching all resumes");
        List<ResumeMetadata> resumes = resumeMatcherService.getAllResumes();
        return ResponseEntity.ok(resumes);
    }
    
    /**
     * Get resume by ID
     */
    @GetMapping("/resumes/{resumeId}")
    public ResponseEntity<ResumeMetadata> getResume(@PathVariable String resumeId) {
        log.info("Fetching resume: {}", resumeId);
        ResumeMetadata resume = resumeMatcherService.getResume(resumeId);
        if (resume == null) {
            return ResponseEntity.notFound().build();
        }
        return ResponseEntity.ok(resume);
    }
    
    /**
     * Health check endpoint
     */
    @GetMapping("/health")
    public ResponseEntity<Map<String, String>> health() {
        Map<String, String> status = Map.of(
            "status", "UP",
            "service", "AI Resume Matcher",
            "version", "1.0.0"
        );
        return ResponseEntity.ok(status);
    }
    
    private boolean isValidFileType(String filename) {
        String extension = filename.substring(filename.lastIndexOf('.') + 1).toLowerCase();
        return extension.equals("pdf") || extension.equals("doc") || 
               extension.equals("docx") || extension.equals("txt");
    }
}