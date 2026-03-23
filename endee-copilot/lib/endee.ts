export interface EndeeIndexConfig {
  index_name: string;
  dim: number;
  space_type?: string;
  sparse_model?: string;
}

export interface EndeeVector {
  id: string;
  vector: number[];
  meta?: string;
  filter?: string;
}

export interface EndeeSearchResult {
  id: string;
  distance: number;
  meta: string;
}

export class EndeeClient {
  private baseUrl: string;
  private token: string;

  constructor() {
    this.baseUrl = process.env.ENDEE_API_URL || "http://localhost:8080";
    this.token = process.env.ENDEE_AUTH_TOKEN || "";
  }

  private getHeaders() {
    const headers: Record<string, string> = {
      "Content-Type": "application/json",
    };
    if (this.token) {
      headers["Authorization"] = this.token;
    }
    return headers;
  }

  async createIndex(config: EndeeIndexConfig) {
    const res = await fetch(`${this.baseUrl}/api/v1/index/create`, {
      method: "POST",
      headers: this.getHeaders(),
      body: JSON.stringify({
        index_name: config.index_name,
        dim: config.dim,
        space_type: config.space_type || "cosine",
        sparse_model: config.sparse_model || "None",
      }),
    });
    // In Endee, if index exists it might return 409 or similar, so we handle it gracefully if needed.
    if (!res.ok) {
        if (res.status === 409) {
            console.log(`Index ${config.index_name} already exists. Proceeding.`);
            return;
        }
      const err = await res.text();
      throw new Error(`Failed to create index: ${err}`);
    }
  }

  async insertVectors(indexName: string, vectors: EndeeVector[]) {
    // Endee expects either an array of objects or single object
    const res = await fetch(`${this.baseUrl}/api/v1/index/${indexName}/vector/insert`, {
      method: "POST",
      headers: this.getHeaders(),
      body: JSON.stringify(vectors),
    });

    if (!res.ok) {
      const err = await res.text();
      throw new Error(`Failed to insert vectors: ${err}`);
    }
  }

  async searchVectors(indexName: string, queryVector: number[], k: number = 5): Promise<EndeeSearchResult[]> {
    const res = await fetch(`${this.baseUrl}/api/v1/index/${indexName}/search`, {
      method: "POST",
      headers: this.getHeaders(),
      body: JSON.stringify({
        k,
        vector: queryVector,
        include_vectors: false,
      }),
    });

    if (!res.ok) {
      const err = await res.text();
      throw new Error(`Failed to search vectors: ${err}`);
    }

    const data = await res.json();
    // Assuming Endee returns { results: [{ id, distance, meta }, ...] }
    return data.results || [];
  }
}

export const endeeDb = new EndeeClient();
