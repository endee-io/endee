package com.resumematcher.dto.request;

import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import lombok.Data;

@Data
public class JobDescriptionRequest {
    @NotBlank(message = "Job description cannot be empty")
    private String jobDescription;
    
    @Min(value = 1, message = "TopK must be at least 1")
    @Max(value = 100, message = "TopK cannot exceed 100")
    private int topK = 10;
}