package guru.springframework.springaifunctions.services;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import guru.springframework.springaifunctions.dto.WeatherDTO;
import org.springframework.stereotype.Service;

import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URL;

@Service
public class WeatherServiceImpl implements WeatherService {

  /**
   * @param city 
   * @return
   */
  @Override
  public String getWeather(String city) throws IOException {
    URL url = new URL("https://api.api-ninjas.com/v1/weather/" + city);
    HttpURLConnection connection = (HttpURLConnection) url.openConnection();
    connection.setRequestProperty("accept", "application/json");
    InputStream responseStream = connection.getInputStream();
    ObjectMapper mapper = new ObjectMapper();
    JsonNode root = mapper.readTree(responseStream);

    return root.path("fact").asText();
  }
}
