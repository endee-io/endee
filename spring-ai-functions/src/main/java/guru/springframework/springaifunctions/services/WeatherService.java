package guru.springframework.springaifunctions.services;

import guru.springframework.springaifunctions.dto.WeatherDTO;

import java.io.IOException;
import java.net.MalformedURLException;

public interface WeatherService {
  String getWeather(String city) throws IOException;
}
