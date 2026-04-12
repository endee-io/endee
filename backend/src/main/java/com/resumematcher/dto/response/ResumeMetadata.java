package com.resumematcher.dto.response;

import lombok.Builder;
import lombok.Data;
import java.time.LocalDateTime;

@Data
@Builder
public class ResumeMetadata {
    private String id;
    private String filename;
    private String candidateName;
    private String email;
    private String[] skills;
    private LocalDateTime uploadedAt;
    private Long fileSize;
}