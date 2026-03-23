import { NextResponse } from "next/server";
import { endeeDb } from "@/lib/endee";
import { openai, generateEmbedding, MODEL } from "@/lib/llm";

export async function POST(req: Request) {
  try {
    const { messages, indexName = "copilot_docs" } = await req.json();

    if (!messages || !Array.isArray(messages) || messages.length === 0) {
      return NextResponse.json({ error: "Invalid messages array" }, { status: 400 });
    }

    // Get the latest user message to run vector search
    const lastUserMessage = messages.filter((m) => m.role === "user").pop();
    let contextStr = "";

    if (lastUserMessage) {
      try {
        const queryEmbedding = await generateEmbedding(lastUserMessage.content);
        
        // Search Endee db for relevant context
        const searchResults = await endeeDb.searchVectors(indexName, queryEmbedding, 5);
        
        if (searchResults && searchResults.length > 0) {
          const contextChunks = searchResults.map((result) => {
            try {
              const metaPayload = JSON.parse(result.meta);
              return `[Source: ${metaPayload.source}]\n${metaPayload.content}`;
            } catch (e) {
              return result.meta;
            }
          });
          
          contextStr = `\n\n=== RELEVANT CONTEXT ===\n${contextChunks.join("\n\n---\n\n")}\n========================\n`;
        }
      } catch (err: any) {
        console.error("Vector search failed, continuing without context:", err.message);
      }
    }

    // Prepare system prompt with context
    const systemPromptMessage = {
      role: "system",
      content: `You are an intelligent Copilot. Answer the user's questions based on the given context. If the context does not contain the answer, say "I don't have enough information to answer that based on the provided documents", but try your best to synthesize the retrieved context. Format your responses in Markdown. ${contextStr}`
    };

    // Replace or prepend the system message
    const hasSystem = messages[0]?.role === "system";
    const finalMessages = hasSystem 
      ? [systemPromptMessage, ...messages.slice(1)]
      : [systemPromptMessage, ...messages];

    const stream = await openai.chat.completions.create({
      model: MODEL,
      messages: finalMessages,
      stream: true,
    });

    return new Response(stream.toReadableStream(), {
      headers: {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        "Connection": "keep-alive"
      }
    });

  } catch (error: any) {
    console.error("Chat error:", error);
    return NextResponse.json({ error: error.message || "Something went wrong" }, { status: 500 });
  }
}
