'use client'

import { useCallback, useEffect } from 'react'
import ReactFlow, {
  Node,
  Edge,
  Background,
  Controls,
  MiniMap,
  useNodesState,
  useEdgesState,
  ConnectionMode,
  Panel,
} from 'reactflow'
import { GraphNode, GraphEdge } from '@/types'
import { Loader2, Brain } from 'lucide-react'

interface KnowledgeGraphProps {
  nodes: GraphNode[]
  edges: GraphEdge[]
  onNodeClick: (node: GraphNode) => void
  isLoading: boolean
}

export default function KnowledgeGraph({
  nodes: graphNodes,
  edges: graphEdges,
  onNodeClick,
  isLoading,
}: KnowledgeGraphProps) {
  const [nodes, setNodes, onNodesChange] = useNodesState([])
  const [edges, setEdges, onEdgesChange] = useEdgesState([])

  // Transform graph data to React Flow format
  const transformToReactFlowNodes = useCallback((graphNodes: GraphNode[]): Node[] => {
    return graphNodes.map((node, index) => ({
      id: node.id,
      type: 'default',
      position: {
        // Simple force-directed layout approximation
        x: Math.cos(index / graphNodes.length * Math.PI * 2) * 300 + 400,
        y: Math.sin(index / graphNodes.length * Math.PI * 2) * 300 + 300,
      },
      data: {
        ...node,
      },
      style: {
        background: 'linear-gradient(135deg, #667eea 0%, #764ba2 100%)',
        color: 'white',
        border: '2px solid #4c51bf',
        borderRadius: '8px',
        padding: '10px',
        fontSize: '12px',
        width: 180,
      },
    }))
  }, [])

  const transformToReactFlowEdges = useCallback((graphEdges: GraphEdge[]): Edge[] => {
    return graphEdges.map((edge) => ({
      id: `${edge.source}-${edge.target}`,
      source: edge.source,
      target: edge.target,
      type: 'smoothstep',
      animated: edge.similarity > 0.85,
      style: {
        stroke: `rgba(99, 102, 241, ${edge.similarity})`,
        strokeWidth: Math.max(1, edge.similarity * 3),
      },
      label: edge.similarity.toFixed(2),
      labelStyle: {
        fontSize: '10px',
        fill: '#94a3b8',
      },
    }))
  }, [])

  // Update React Flow nodes and edges when graph data changes
  useEffect(() => {
    if (graphNodes.length > 0) {
      const flowNodes = transformToReactFlowNodes(graphNodes)
      const flowEdges = transformToReactFlowEdges(graphEdges)
      setNodes(flowNodes)
      setEdges(flowEdges)
    }
  }, [graphNodes, graphEdges, transformToReactFlowNodes, transformToReactFlowEdges, setNodes, setEdges])

  const handleNodeClick = useCallback(
    (_: React.MouseEvent, node: Node) => {
      const graphNode = graphNodes.find((n) => n.id === node.id)
      if (graphNode) {
        onNodeClick(graphNode)
      }
    },
    [graphNodes, onNodeClick]
  )

  if (isLoading) {
    return (
      <div className="w-full h-full flex items-center justify-center bg-dark-900">
        <div className="text-center">
          <Loader2 className="w-12 h-12 text-primary-500 animate-spin mx-auto mb-4" />
          <p className="text-dark-400">Building knowledge graph...</p>
        </div>
      </div>
    )
  }

  if (graphNodes.length === 0) {
    return (
      <div className="w-full h-full flex items-center justify-center bg-dark-900">
        <div className="text-center max-w-md">
          <div className="w-20 h-20 bg-dark-800 rounded-full flex items-center justify-center mx-auto mb-4">
            <Brain className="w-10 h-10 text-dark-600" />
          </div>
          <h3 className="text-xl font-semibold text-white mb-2">
            No Knowledge Yet
          </h3>
          <p className="text-dark-400">
            Upload documents to begin building your intelligence graph.
          </p>
        </div>
      </div>
    )
  }

  return (
    <div className="w-full h-full">
      <ReactFlow
        nodes={nodes}
        edges={edges}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onNodeClick={handleNodeClick}
        connectionMode={ConnectionMode.Loose}
        fitView
        attributionPosition="bottom-left"
      >
        <Background color="#334155" gap={16} />
        <Controls className="bg-dark-800 border border-dark-700" />
        <MiniMap
          nodeColor="#667eea"
          maskColor="rgba(0, 0, 0, 0.6)"
          className="bg-dark-800 border border-dark-700"
        />
        <Panel position="top-right" className="bg-dark-800 border border-dark-700 rounded-lg p-3 text-sm">
          <div className="space-y-1 text-dark-300">
            <div><strong>{graphNodes.length}</strong> concepts</div>
            <div><strong>{graphEdges.length}</strong> connections</div>
          </div>
        </Panel>
      </ReactFlow>
    </div>
  )
}
