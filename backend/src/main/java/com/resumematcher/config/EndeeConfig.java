package com.resumematcher.config;

import lombok.Data;
import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.context.annotation.Configuration;

@Data
@Configuration
@ConfigurationProperties(prefix = "endee")
public class EndeeConfig {
    private String baseUrl;
    private String indexName;
    private int vectorDimension;
    private String similarityMetric;
}