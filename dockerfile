FROM python:3.10-slim

WORKDIR /usr/src

RUN pip install fastapi uvicorn nest_asyncio deepeval

WORKDIR /llm-response-evaluator

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000", "--reload"]
