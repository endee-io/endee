import OpenAI from "openai";

// If using Ollama, provide OPENAI_BASE_URL=http://localhost:11434/v1
export const openai = new OpenAI({
  apiKey: process.env.OPENAI_API_KEY || "sk-dummy-key",
  baseURL: process.env.OPENAI_BASE_URL,
});

export const MODEL = process.env.OPENAI_MODEL || "gpt-4o-mini";
export const EMBEDDING_MODEL = process.env.OPENAI_EMBEDDING_MODEL || "text-embedding-3-small";

export async function generateEmbedding(text: string): Promise<number[]> {
  const response = await openai.embeddings.create({
    model: EMBEDDING_MODEL,
    input: text,
  });

  return response.data[0].embedding;
}
