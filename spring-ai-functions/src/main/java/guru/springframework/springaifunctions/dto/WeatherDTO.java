package guru.springframework.springaifunctions.dto;

public record WeatherDTO (Double windSpeed, Integer windDegrees, Double temp, Double humidity,
                          Long sunset, Double minTemp, Integer cloudPct, Double feelsLike,
                          Long sunrise, Double maxTemp)         {
}

/*
{
  "wind_speed": 5.66,
  "wind_degrees": 210,
  "temp": 7,
  "humidity": 87,
  "sunset": 1615658463,
  "min_temp": 7,
  "cloud_pct": 75,
  "feels_like": 2,
  "sunrise": 1615616341,
  "max_temp": 8
}
 */
