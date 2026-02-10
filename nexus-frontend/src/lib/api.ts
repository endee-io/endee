import axios from 'axios'
import { GraphData, QueryResult, DocumentUploadResponse, NodeDetails } from '@/types'

const API_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8000'

const api = axios.create({
  baseURL: API_URL,
  headers: {
    'Content-Type': 'application/json',
  },
  timeout: 30000,
})

export const apiClient = {
  // Health check
  async healthCheck() {
    const response = await api.get('/health')
    return response.data
  },

  // Initialize system
  async initialize() {
    const response = await api.post('/api/initialize')
    return response.data
  },

  // Upload document
  async uploadDocument(file: File): Promise<DocumentUploadResponse> {
    const formData = new FormData()
    formData.append('file', file)

    const response = await api.post('/api/documents/upload', formData, {
      headers: {
        'Content-Type': 'multipart/form-data',
      },
    })

    return response.data
  },

  // Get knowledge graph
  async getGraph(
    similarityThreshold: number = 0.7,
    maxNodes: number = 100
  ): Promise<GraphData> {
    const response = await api.get('/api/graph', {
      params: {
        similarity_threshold: similarityThreshold,
        max_nodes: maxNodes,
      },
    })

    return response.data
  },

  // Semantic query
  async query(
    query: string,
    topK: number = 10,
    similarityThreshold: number = 0.7
  ): Promise<QueryResult> {
    const response = await api.post('/api/query', {
      query,
      top_k: topK,
      similarity_threshold: similarityThreshold,
    })

    return response.data
  },

  // Get node details
  async getNodeDetails(nodeId: string): Promise<NodeDetails> {
    const response = await api.get(`/api/node/${nodeId}`)
    return response.data
  },

  // Get statistics
  async getStats() {
    const response = await api.get('/api/stats')
    return response.data
  },

  // Delete document
  async deleteDocument(documentId: string) {
    const response = await api.delete(`/api/documents/${documentId}`)
    return response.data
  },
}
