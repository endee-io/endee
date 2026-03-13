"""
Multi-provider LLM client — supports Gemini, OpenAI, and Claude.

Usage:
    client = LLMClient()           # reads provider from settings
    response = client.generate("Your prompt here")
"""

import logging
from abc import ABC, abstractmethod
from typing import Optional

from config.settings import settings
from prompts.templates import RAG_SYSTEM_PROMPT, build_rag_prompt

logger = logging.getLogger(__name__)


# ── Abstract base ──────────────────────────────────────────────────────

class BaseLLM(ABC):
    """Interface that every LLM provider must implement."""

    @abstractmethod
    def generate(self, prompt: str, system_prompt: str = "") -> str:
        """Send a prompt to the LLM and return the text response."""
        ...


# ── Gemini ─────────────────────────────────────────────────────────────

class GeminiLLM(BaseLLM):
    """Google Gemini via the google-genai SDK."""

    def __init__(self, api_key: str, model: str):
        try:
            from google import genai
        except ImportError:
            raise ImportError("Install the Gemini SDK: pip install google-genai")

        self.client = genai.Client(api_key=api_key)
        self.model = model
        logger.info(f"Gemini client initialized (model={model})")

    def generate(self, prompt: str, system_prompt: str = "") -> str:
        from google.genai import types

        response = self.client.models.generate_content(
            model=self.model,
            contents=prompt,
            config=types.GenerateContentConfig(
                system_instruction=system_prompt or None,
                temperature=0.3,
                max_output_tokens=2048,
            ),
        )
        return response.text


# ── OpenAI ─────────────────────────────────────────────────────────────

class OpenAILLM(BaseLLM):
    """OpenAI GPT models via the openai SDK."""

    def __init__(self, api_key: str, model: str):
        try:
            from openai import OpenAI
        except ImportError:
            raise ImportError("Install the OpenAI SDK: pip install openai")

        self.client = OpenAI(api_key=api_key)
        self.model = model
        logger.info(f"OpenAI client initialized (model={model})")

    def generate(self, prompt: str, system_prompt: str = "") -> str:
        messages = []
        if system_prompt:
            messages.append({"role": "system", "content": system_prompt})
        messages.append({"role": "user", "content": prompt})

        response = self.client.chat.completions.create(
            model=self.model,
            messages=messages,
            temperature=0.3,
            max_tokens=2048,
        )
        return response.choices[0].message.content


# ── Claude ─────────────────────────────────────────────────────────────

class ClaudeLLM(BaseLLM):
    """Anthropic Claude via the anthropic SDK."""

    def __init__(self, api_key: str, model: str):
        try:
            import anthropic
        except ImportError:
            raise ImportError("Install the Anthropic SDK: pip install anthropic")

        self.client = anthropic.Anthropic(api_key=api_key)
        self.model = model
        logger.info(f"Claude client initialized (model={model})")

    def generate(self, prompt: str, system_prompt: str = "") -> str:
        message = self.client.messages.create(
            model=self.model,
            max_tokens=2048,
            system=system_prompt or "You are a helpful assistant.",
            messages=[{"role": "user", "content": prompt}],
            temperature=0.3,
        )
        return message.content[0].text


# ── Factory ────────────────────────────────────────────────────────────

_PROVIDERS = {
    "gemini": lambda: GeminiLLM(
        api_key=settings.gemini_api_key,
        model=settings.active_llm_model,
    ),
    "openai": lambda: OpenAILLM(
        api_key=settings.openai_api_key,
        model=settings.active_llm_model,
    ),
    "claude": lambda: ClaudeLLM(
        api_key=settings.anthropic_api_key,
        model=settings.active_llm_model,
    ),
}


class LLMClient:
    """
    Unified LLM client — picks the provider from settings automatically.

    Example:
        client = LLMClient()
        answer = client.generate("Explain quantum computing.")
    """

    def __init__(self, provider: Optional[str] = None):
        provider = (provider or settings.llm_provider).lower()

        factory = _PROVIDERS.get(provider)
        if factory is None:
            raise ValueError(
                f"Unsupported LLM provider: '{provider}'. "
                f"Choose from: {', '.join(_PROVIDERS)}"
            )

        self._llm: BaseLLM = factory()
        self.provider = provider

    def generate(self, prompt: str, system_prompt: str = "") -> str:
        """Generate a response from the LLM."""
        return self._llm.generate(prompt, system_prompt)


# ── Convenience function ───────────────────────────────────────────────

def answer_question(question: str, context: str) -> str:
    """
    High-level function: combine prompt template + context + question,
    send to the configured LLM, and return the answer.

    Args:
        question:  User's question.
        context:   Formatted context string from the retriever.

    Returns:
        The LLM's answer as a string.
    """
    prompt = build_rag_prompt(question, context)
    client = LLMClient()
    answer = client.generate(prompt, system_prompt=RAG_SYSTEM_PROMPT)
    return answer
