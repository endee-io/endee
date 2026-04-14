package guru.springframework.springaifunctions.functions;

import guru.springframework.springaifunctions.model.WeatherRequest;
import guru.springframework.springaifunctions.model.WeatherResponse;
import org.springframework.web.client.RestClient;

import java.util.function.Function;

public class WeatherServiceFunction implements Function<WeatherRequest, WeatherResponse> {

  public static final String WEATHER_URL = "https://api.api-ninjas.com/v1/weather";

  private final String apiNinjasKey;

  public WeatherServiceFunction(String apiNinjasKey) {

    this.apiNinjasKey = apiNinjasKey;
  }

  /**
   * @param weatherRequest the function argument 
   * @return WeatherResponse dto
   */
  @Override
  public WeatherResponse apply(WeatherRequest weatherRequest) {
    RestClient restClient = RestClient.builder()
        .baseUrl(WEATHER_URL)
        .defaultHeaders(httpHeaders -> {
          httpHeaders.set("X-Api-Key", apiNinjasKey);
          httpHeaders.set("Accept", "application/json");
          httpHeaders.set("Content-Type", "application/json");
        }).build();

    return restClient.get().uri(uriBuilder -> {
      System.out.println("Building URI for weather request: " + weatherRequest);

      uriBuilder.queryParam("city", weatherRequest.location());

      if (weatherRequest.state() != null && !weatherRequest.state().isBlank()) {
        uriBuilder.queryParam("state", weatherRequest.state());
      }
      if (weatherRequest.country() != null && !weatherRequest.country().isBlank()) {
        uriBuilder.queryParam("country", weatherRequest.country());
      }
      return uriBuilder.build();
    }).retrieve().body(WeatherResponse.class);
  }
}
