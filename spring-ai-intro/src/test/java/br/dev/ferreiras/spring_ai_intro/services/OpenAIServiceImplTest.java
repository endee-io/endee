package br.dev.ferreiras.spring_ai_intro.services;

import org.junit.jupiter.api.Test;
import org.junit.platform.commons.logging.Logger;
import org.junit.platform.commons.logging.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;

import static org.junit.jupiter.api.Assertions.*;

@SpringBootTest
class OpenAIServiceImplTest {

  private static final Logger logger = LoggerFactory.getLogger(OpenAIServiceImplTest.class);

  @Autowired
  OpenAIService openAIService;

  @Test
  void getAnswer() {

    String answer = openAIService.getAnswer("Write a python script to output numbers from 1 to 100");
    System.out.println("Got the answer!!");
    System.out.println(answer);
  }
}