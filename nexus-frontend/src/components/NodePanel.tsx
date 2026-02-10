'use client'

import { useState, useEffect } from 'react'
import { GraphNode } from '@/types'
import { X, FileText, Link as LinkIcon, Loader2 } from 'lucide-react'
import { apiClient } from '@/lib/api'

interface NodePanelProps {
  node: GraphNode
  onClose: () => void
}

export default function NodePanel({ node, onClose }: NodePanelProps) {
  const [nodeDetails, setNodeDetails] = useState<any>(null)
  const [isLoading, setIsLoading] = useState(true)

  useEffect(() => {
    async function fetchDetails() {
      setIsLoading(true)
      try {
        const details = await apiClient.getNodeDetails(node.id)
        setNodeDetails(details)
      } catch (error) {
        console.error('Failed to fetch node details:', error)
      } finally {
        setIsLoading(false)
      }
    }

    fetchDetails()
  }, [node.id])

  return (
    <div className="h-full flex flex-col">
      {/* Header */}
      <div className="border-b border-dark-700 p-4 flex items-start justify-between">
        <div className="flex-1">
          <h2 className="text-lg font-semibold text-white mb-1">Node Details</h2>
          <p className="text-sm text-dark-400 line-clamp-2">{node.label}</p>
        </div>
        <button
          onClick={onClose}
          className="ml-2 p-2 hover:bg-dark-700 rounded-lg transition-colors"
        >
          <X className="w-5 h-5 text-dark-400" />
        </button>
      </div>

      {/* Content */}
      <div className="flex-1 overflow-y-auto p-4 space-y-6">
        {isLoading ? (
          <div className="flex items-center justify-center py-12">
            <Loader2 className="w-8 h-8 text-primary-500 animate-spin" />
          </div>
        ) : (
          <>
            {/* Summary Section */}
            <div>
              <h3 className="text-sm font-medium text-dark-300 mb-2 flex items-center gap-2">
                <FileText className="w-4 h-4" />
                Summary
              </h3>
              <div className="bg-dark-700 rounded-lg p-4">
                <p className="text-sm text-dark-200 leading-relaxed">
                  {node.summary || 'No summary available'}
                </p>
              </div>
            </div>

            {/* Metadata Section */}
            <div>
              <h3 className="text-sm font-medium text-dark-300 mb-2">Metadata</h3>
              <div className="bg-dark-700 rounded-lg p-4 space-y-2 text-sm">
                <div className="flex justify-between">
                  <span className="text-dark-400">Node ID:</span>
                  <span className="text-dark-200 font-mono text-xs">{node.id.slice(0, 12)}...</span>
                </div>
                <div className="flex justify-between">
                  <span className="text-dark-400">Document:</span>
                  <span className="text-dark-200">{node.metadata.filename || 'Unknown'}</span>
                </div>
                {node.metadata.chunk_index !== undefined && (
                  <div className="flex justify-between">
                    <span className="text-dark-400">Chunk:</span>
                    <span className="text-dark-200">#{node.metadata.chunk_index}</span>
                  </div>
                )}
              </div>
            </div>

            {/* Related Concepts */}
            {nodeDetails?.related_nodes && nodeDetails.related_nodes.length > 0 && (
              <div>
                <h3 className="text-sm font-medium text-dark-300 mb-2 flex items-center gap-2">
                  <LinkIcon className="w-4 h-4" />
                  Related Concepts ({nodeDetails.related_nodes.length})
                </h3>
                <div className="space-y-2">
                  {nodeDetails.related_nodes.map((related: any) => (
                    <div
                      key={related.node_id}
                      className="bg-dark-700 rounded-lg p-3 hover:bg-dark-600 transition-colors cursor-pointer"
                    >
                      <div className="flex items-start justify-between gap-2">
                        <p className="text-sm text-dark-200 flex-1">{related.label}</p>
                        <span className="text-xs text-primary-400 font-medium">
                          {(related.similarity * 100).toFixed(0)}%
                        </span>
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            )}

            {/* Connection Stats */}
            {nodeDetails?.connection_count !== undefined && (
              <div>
                <h3 className="text-sm font-medium text-dark-300 mb-2">Connection Stats</h3>
                <div className="bg-dark-700 rounded-lg p-4">
                  <div className="flex items-center justify-between">
                    <span className="text-dark-400">Total Connections:</span>
                    <span className="text-2xl font-bold text-primary-400">
                      {nodeDetails.connection_count}
                    </span>
                  </div>
                </div>
              </div>
            )}

            {/* Intelligence Insights */}
            <div>
              <h3 className="text-sm font-medium text-dark-300 mb-2">Intelligence Insights</h3>
              <div className="bg-gradient-to-br from-primary-500/10 to-purple-500/10 border border-primary-500/20 rounded-lg p-4 space-y-2">
                <div className="flex items-start gap-2">
                  <div className="w-1.5 h-1.5 rounded-full bg-primary-400 mt-1.5"></div>
                  <p className="text-sm text-dark-300">
                    This concept is highly connected in your knowledge graph
                  </p>
                </div>
                <div className="flex items-start gap-2">
                  <div className="w-1.5 h-1.5 rounded-full bg-primary-400 mt-1.5"></div>
                  <p className="text-sm text-dark-300">
                    Consider exploring related nodes to expand understanding
                  </p>
                </div>
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
