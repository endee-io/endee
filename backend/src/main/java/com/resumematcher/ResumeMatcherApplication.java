package com.resumematcher;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.scheduling.annotation.EnableAsync;

@SpringBootApplication
@EnableAsync
@EnableConfigurationProperties
public class ResumeMatcherApplication {
    public static void main(String[] args) {
        SpringApplication.run(ResumeMatcherApplication.class, args);
    }
}