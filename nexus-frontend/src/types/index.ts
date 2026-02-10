export interface GraphNode {
  id: string
  label: string
  summary: string
  embedding_id: string
  document_id: string
  metadata: Record<string, any>
}

export interface GraphEdge {
  source: string
  target: string
  similarity: number
  relationship_type: string
}

export interface GraphData {
  nodes: GraphNode[]
  edges: GraphEdge[]
  stats: {
    total_nodes: number
    total_edges: number
    unique_documents: number
    avg_connections_per_node: number
    avg_similarity: number
    graph_density: number
  }
}

export interface QueryResult {
  query: string
  nodes: GraphNode[]
  edges: GraphEdge[]
  execution_time_ms: number
}

export interface DocumentUploadResponse {
  document_id: string
  filename: string
  chunks_created: number
  status: string
}

export interface NodeDetails extends GraphNode {
  related_nodes: Array<{
    node_id: string
    label: string
    similarity: number
  }>
  connection_count: number
}
