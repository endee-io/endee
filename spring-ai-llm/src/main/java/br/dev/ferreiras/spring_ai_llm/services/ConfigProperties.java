package br.dev.ferreiras.spring_ai_llm.services;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.context.annotation.Configuration;


@Configuration
@ConfigurationProperties(prefix = "evaluation.geturl")
public class ConfigProperties {

  private static String apiUrl = "http://127.0.0.1:8000/";

  public ConfigProperties() {
    // Default constructor
  }

  public static String getApiUrl() {
    return apiUrl;
  }

  public void setApiUrl(String apiUrl) {
    if (apiUrl == null || (!apiUrl.startsWith("http://") && !apiUrl.startsWith("https://"))) {
      throw new IllegalArgumentException("Invalid API URL: " + apiUrl);
    }
    ConfigProperties.apiUrl = apiUrl;
  }
}

