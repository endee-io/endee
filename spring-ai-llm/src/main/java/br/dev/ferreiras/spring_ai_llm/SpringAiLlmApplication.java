package br.dev.ferreiras.spring_ai_llm;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;

import java.time.ZonedDateTime;

@SpringBootApplication
public class SpringAiLlmApplication implements CommandLineRunner {

	private static final Logger logger = LoggerFactory.getLogger(SpringAiLlmApplication.class);

	public static void main(String[] args) {
		SpringApplication.run(SpringAiLlmApplication.class, args);
	}

	/**
	 * @param args - no args
	 * @throws Exception on error
	 */
	@Override
	public void run(String... args) throws Exception {
		ZonedDateTime zonedDateTime = ZonedDateTime.now(ZonedDateTime.now().getZone());
		int cores = Runtime.getRuntime().availableProcessors();
		long totalMemory = Runtime.getRuntime().totalMemory()/1_000_000L;
		String version = System.getProperty("java.version");

		if (logger.isInfoEnabled()) {
			logger.info("Spring AI LLM Restful API started running at {},running java version {}, " +
									"on top of {} cores and {}MB of RAM",
					zonedDateTime,
					version,
					cores,
					totalMemory
			);
		}



	}
}
