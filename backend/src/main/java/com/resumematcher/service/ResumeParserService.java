package com.resumematcher.service;

import lombok.extern.slf4j.Slf4j;
import org.apache.tika.exception.TikaException;
import org.apache.tika.metadata.Metadata;
import org.apache.tika.parser.ParseContext;
import org.apache.tika.parser.pdf.PDFParser;
import org.apache.tika.parser.txt.TXTParser;
import org.apache.tika.parser.microsoft.ooxml.WordprocessingMLParser;
import org.apache.tika.sax.BodyContentHandler;
import org.springframework.stereotype.Service;
import org.springframework.web.multipart.MultipartFile;
import org.xml.sax.SAXException;

import java.io.IOException;
import java.io.InputStream;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

@Slf4j
@Service
public class ResumeParserService {
    
    /**
     * Extracts text from uploaded resume file
     */
    public String extractText(MultipartFile file) {
        String filename = file.getOriginalFilename();
        if (filename == null) {
            throw new IllegalArgumentException("File name is missing");
        }
        
        String extension = getFileExtension(filename);
        
        try (InputStream inputStream = file.getInputStream()) {
            BodyContentHandler handler = new BodyContentHandler(-1); // No character limit
            Metadata metadata = new Metadata();
            ParseContext context = new ParseContext();
            
            switch (extension.toLowerCase()) {
                case "pdf":
                    PDFParser parser = new PDFParser();
                    parser.parse(inputStream, handler, metadata, context);
                    break;
                case "doc":
                case "docx":
                    WordprocessingMLParser wordParser = new WordprocessingMLParser();
                    wordParser.parse(inputStream, handler, metadata, context);
                    break;
                case "txt":
                    TXTParser txtParser = new TXTParser();
                    txtParser.parse(inputStream, handler, metadata, context);
                    break;
                default:
                    throw new IllegalArgumentException("Unsupported file format: " + extension);
            }
            
            String extractedText = handler.toString();
            log.info("Successfully extracted text from {} ({} characters)", filename, extractedText.length());
            return cleanText(extractedText);
            
        } catch (IOException | SAXException | TikaException e) {
            log.error("Failed to extract text from resume: {}", e.getMessage());
            throw new RuntimeException("Failed to parse resume file: " + e.getMessage());
        }
    }
    
    /**
     * Extracts email from resume text
     */
    public String extractEmail(String text) {
        // Common email regex pattern
        Pattern emailPattern = Pattern.compile("[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}");
        Matcher matcher = emailPattern.matcher(text);
        
        if (matcher.find()) {
            return matcher.group();
        }
        return null;
    }
    
    /**
     * Extracts candidate name (simplified - looks for common name patterns)
     */
    public String extractName(String text) {
        // Look for patterns like "Name: John Doe" or common name formats
        Pattern namePattern = Pattern.compile("(?:Name|Candidate)[:\\s]+([A-Z][a-z]+\\s+[A-Z][a-z]+)", Pattern.MULTILINE);
        Matcher matcher = namePattern.matcher(text);
        
        if (matcher.find()) {
            return matcher.group(1);
        }
        
        // Fallback: first line might contain name
        String firstLine = text.split("\n")[0].trim();
        if (firstLine.length() < 50 && firstLine.matches(".*[A-Z][a-z]+\\s+[A-Z][a-z]+.*")) {
            return firstLine;
        }
        
        return null;
    }
    
    /**
     * Basic skill extraction (can be enhanced with NLP)
     */
    public String[] extractSkills(String text) {
        String[] commonSkills = {
            "Java", "Python", "JavaScript", "React", "Spring Boot", "Node.js",
            "AWS", "Docker", "Kubernetes", "SQL", "MongoDB", "PostgreSQL",
            "Machine Learning", "AI", "TensorFlow", "PyTorch", "REST API",
            "Microservices", "Git", "CI/CD", "Agile", "Scrum"
        };
        
        java.util.List<String> foundSkills = new java.util.ArrayList<>();
        String lowerText = text.toLowerCase();
        
        for (String skill : commonSkills) {
            if (lowerText.contains(skill.toLowerCase())) {
                foundSkills.add(skill);
            }
        }
        
        return foundSkills.toArray(new String[0]);
    }
    
    private String getFileExtension(String filename) {
        int lastDot = filename.lastIndexOf('.');
        if (lastDot == -1) {
            return "";
        }
        return filename.substring(lastDot + 1);
    }
    
    private String cleanText(String text) {
        // Remove excessive whitespace
        text = text.replaceAll("\\s+", " ");
        // Remove special characters but keep basic punctuation
        text = text.replaceAll("[^\\w\\s@.-]", " ");
        // Normalize whitespace
        text = text.trim();
        return text;
    }
}