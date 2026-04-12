package com.resumematcher.dto.response;

import lombok.Builder;
import lombok.Data;

@Data
@Builder
public class MatchResult {
    private String resumeId;
    private String candidateName;
    private String filename;
    private String email;
    private String[] skills;
    private double similarityScore;
    private String matchGrade;
}