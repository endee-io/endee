import { NextResponse } from "next/server";
import { endeeDb } from "@/lib/endee";
import { generateEmbedding } from "@/lib/llm";

// Simple text chunker
function splitText(text: string, chunkSize: number = 1000, overlap: number = 200): string[] {
  const words = text.split(/\s+/);
  const chunks: string[] = [];
  let currentChunk: string[] = [];
  let currentLength = 0;

  for (let i = 0; i < words.length; i++) {
    const word = words[i];
    if (currentLength + word.length > chunkSize && currentChunk.length > 0) {
      chunks.push(currentChunk.join(" "));
      
      // Calculate overlap back
      const overlapWords: string[] = [];
      let overlapLength = 0;
      for (let j = currentChunk.length - 1; j >= 0; j--) {
        if (overlapLength + currentChunk[j].length <= overlap) {
          overlapWords.unshift(currentChunk[j]);
          overlapLength += currentChunk[j].length + 1;
        } else {
          break;
        }
      }
      currentChunk = [...overlapWords];
      currentLength = overlapLength;
    }
    
    currentChunk.push(word);
    currentLength += word.length + 1; // +1 for space
  }

  if (currentChunk.length > 0) {
    chunks.push(currentChunk.join(" "));
  }

  return chunks;
}

export async function POST(req: Request) {
  try {
    const data = await req.formData();
    const file = data.get("file") as File;
    const indexName = (data.get("indexName") as string) || "copilot_docs";

    if (!file) {
      return NextResponse.json({ error: "No file provided" }, { status: 400 });
    }

    const text = await file.text();
    const chunks = splitText(text, 1000, 200);

    // 1. Create Index if it doesn't exist
    // Using dimension 1536 for text-embedding-3-small and text-embedding-ada-002
    await endeeDb.createIndex({
      index_name: indexName,
      dim: 1536,
      space_type: "cosine"
    }).catch(console.error); // Ignore error if already exists

    // 2. Generate embeddings and prepare vectors
    const vectorsToInsert = [];
    for (let i = 0; i < chunks.length; i++) {
      const chunk = chunks[i];
      const embedding = await generateEmbedding(chunk);
      
      vectorsToInsert.push({
        id: `${file.name}_chunk_${i}_${Date.now()}`,
        vector: embedding,
        meta: JSON.stringify({
          source: file.name,
          content: chunk
        })
      });
    }

    // 3. Insert into Endee DB
    await endeeDb.insertVectors(indexName, vectorsToInsert);

    return NextResponse.json({ 
      success: true, 
      message: `Successfully ingested ${chunks.length} chunks from ${file.name}` 
    });

  } catch (error: any) {
    console.error("Ingestion error:", error);
    return NextResponse.json({ error: error.message || "Failed to ingest document" }, { status: 500 });
  }
}
