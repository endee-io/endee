import { create } from 'zustand'
import { GraphNode, GraphEdge, GraphData } from '@/types'
import { apiClient } from '@/lib/api'

interface GraphStore {
  nodes: GraphNode[]
  edges: GraphEdge[]
  stats: GraphData['stats'] | null
  isLoading: boolean
  error: string | null
  
  // Actions
  fetchGraph: (similarityThreshold?: number, maxNodes?: number) => Promise<void>
  executeQuery: (query: string) => Promise<void>
  uploadDocument: (file: File) => Promise<void>
  clearGraph: () => void
  setError: (error: string | null) => void
}

export const useGraphStore = create<GraphStore>((set, get) => ({
  nodes: [],
  edges: [],
  stats: null,
  isLoading: false,
  error: null,

  fetchGraph: async (similarityThreshold = 0.7, maxNodes = 100) => {
    set({ isLoading: true, error: null })
    
    try {
      const data = await apiClient.getGraph(similarityThreshold, maxNodes)
      
      set({
        nodes: data.nodes,
        edges: data.edges,
        stats: data.stats,
        isLoading: false,
      })
    } catch (error: any) {
      set({
        error: error.message || 'Failed to fetch graph',
        isLoading: false,
      })
    }
  },

  executeQuery: async (query: string) => {
    set({ isLoading: true, error: null })
    
    try {
      const result = await apiClient.query(query)
      
      set({
        nodes: result.nodes,
        edges: result.edges,
        isLoading: false,
      })
    } catch (error: any) {
      set({
        error: error.message || 'Query execution failed',
        isLoading: false,
      })
    }
  },

  uploadDocument: async (file: File) => {
    set({ isLoading: true, error: null })
    
    try {
      await apiClient.uploadDocument(file)
      
      // Refresh graph after upload (with slight delay for processing)
      setTimeout(() => {
        get().fetchGraph()
      }, 2000)
    } catch (error: any) {
      set({
        error: error.message || 'Document upload failed',
        isLoading: false,
      })
    }
  },

  clearGraph: () => {
    set({ nodes: [], edges: [], stats: null })
  },

  setError: (error: string | null) => {
    set({ error })
  },
}))
