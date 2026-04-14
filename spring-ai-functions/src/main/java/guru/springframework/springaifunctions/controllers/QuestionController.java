package guru.springframework.springaifunctions.controllers;


import guru.springframework.springaifunctions.model.Answer;
import guru.springframework.springaifunctions.model.Question;
import guru.springframework.springaifunctions.services.OpenAIService;
import guru.springframework.springaifunctions.services.WeatherService;
import lombok.RequiredArgsConstructor;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

import java.io.IOException;

/**
 * Created by jt, Spring Framework Guru.
 */

@RestController
public class QuestionController {

    private final OpenAIService openAIService;

    private final WeatherService weatherService;

    public QuestionController(OpenAIService openAIService, WeatherService weatherService) {
        this.openAIService = openAIService;
        this.weatherService = weatherService;
    }

    @PostMapping("/weather")
    public Answer askQuestion(@RequestBody Question question) {

        return openAIService.getAnswer(question);
    }

    @GetMapping("/ninjas")
    public ResponseEntity<String> requestWeather(@RequestParam(value="city", defaultValue = "London") String city) throws IOException {

        return ResponseEntity.ok().body(weatherService.getWeather(city));
    }
}
