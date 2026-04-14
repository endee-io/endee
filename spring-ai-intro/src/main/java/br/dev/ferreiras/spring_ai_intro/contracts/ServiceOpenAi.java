package br.dev.ferreiras.spring_ai_intro.contracts;

import br.dev.ferreiras.spring_ai_intro.model.Answer;
import br.dev.ferreiras.spring_ai_intro.model.CapitalRequest;
import br.dev.ferreiras.spring_ai_intro.model.CapitalResponse;
import br.dev.ferreiras.spring_ai_intro.model.Question;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.service.annotation.HttpExchange;
import org.springframework.web.service.annotation.PostExchange;

/*
While the main purpose of @HttpExchange is to abstract HTTP client code with a generated proxy,
the HTTP Interface on which such annotations are placed is a contract neutral to client vs server use.
In addition to simplifying client code, there are also cases where an HTTP Interface may be a convenient
way for servers to expose their API for client access.
This leads to increased coupling between client and server and is often not a good choice, especially for public API’s,
but may be exactly the goal for an internal API.
It is an approach commonly used in Spring Cloud, and it is why @HttpExchange is supported as an alternative to
@RequestMapping for server side handling in controller classes.
@HttpExchange and @RequestMapping have differences.
@RequestMapping can map to any number of requests by path patterns, HTTP methods, and more, while @HttpExchange
declares a single endpoint with a concrete HTTP method, path, and content types.

For method parameters and returns values, generally, @HttpExchange supports a subset of the method parameters
that @RequestMapping does
 */

@HttpExchange("/ai")
public interface ServiceOpenAi {

  @PostExchange
  public Answer askQuestion(@RequestBody Question question) ;

  @PostExchange("/capital")
  public Answer getCapital(@RequestBody CapitalRequest capitalRequest);

  @PostExchange("/capitalJ")
  public CapitalResponse getCapitalJ(@RequestBody CapitalRequest capitalRequest);
}
