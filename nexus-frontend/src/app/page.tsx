'use client'

import { useState, useCallback, useEffect } from 'react'
import KnowledgeGraph from '@/components/KnowledgeGraph'
import ControlPanel from '@/components/ControlPanel'
import NodePanel from '@/components/NodePanel'
import Header from '@/components/Header'
import { useGraphStore } from '@/store/graphStore'
import { GraphNode } from '@/types'

export default function Home() {
  const [selectedNode, setSelectedNode] = useState<GraphNode | null>(null)
  const [showControlPanel, setShowControlPanel] = useState(true)
  const { nodes, edges, isLoading, fetchGraph} = useGraphStore()

  useEffect(() => {
    // Load initial graph on mount
    fetchGraph()
  }, [fetchGraph])

  const handleNodeClick = useCallback((node: GraphNode) => {
    setSelectedNode(node)
  }, [])

  const handleClosePanel = useCallback(() => {
    setSelectedNode(null)
  }, [])

  return (
    <div className="flex flex-col h-screen bg-dark-900 text-white">
      <Header 
        onToggleControlPanel={() => setShowControlPanel(!showControlPanel)}
        showControlPanel={showControlPanel}
      />
      
      <div className="flex flex-1 overflow-hidden relative">
        {/* Control Panel - Left Sidebar (Collapsible on mobile) */}
        <aside 
          className={`
            absolute md:relative
            w-full md:w-80 h-full
            bg-dark-800 border-r border-dark-700 overflow-y-auto
            transition-transform duration-300 ease-out
            z-30
            ${showControlPanel ? 'translate-x-0' : '-translate-x-full md:translate-x-0'}
          `}
        >
          <ControlPanel onClose={() => setShowControlPanel(false)} />
        </aside>

        {/* Mobile Overlay */}
        {showControlPanel && (
          <div
            className="absolute md:hidden w-full h-full bg-black/50 z-20"
            onClick={() => setShowControlPanel(false)}
          />
        )}

        {/* Main Graph Area */}
        <main className="flex-1 relative w-full md:flex-auto">
          <KnowledgeGraph
            nodes={nodes}
            edges={edges}
            onNodeClick={handleNodeClick}
            isLoading={isLoading}
          />
        </main>

        {/* Node Details Panel - Right Side (Modal on mobile) */}
        {selectedNode && (
          <>
            {/* Desktop - Side Panel */}
            <aside className="hidden lg:flex w-96 bg-dark-800 border-l border-dark-700 overflow-y-auto animate-fade-in flex-col">
              <NodePanel node={selectedNode} onClose={handleClosePanel} />
            </aside>

            {/* Mobile - Modal */}
            <div className="lg:hidden fixed inset-0 z-40 flex flex-col">
              {/* Mobile Overlay */}
              <div
                className="absolute inset-0 bg-black/70"
                onClick={handleClosePanel}
              />
              
              {/* Modal Content - Bottom Sheet */}
              <div className="absolute bottom-0 w-full max-h-[80vh] bg-dark-800 border-t border-dark-700 rounded-t-2xl overflow-y-auto animate-fade-in z-50">
                <div className="sticky top-0 bg-dark-800 p-4 border-b border-dark-700 flex items-center justify-between">
                  <h2 className="text-lg font-semibold text-white">Node Details</h2>
                  <button
                    onClick={handleClosePanel}
                    className="p-2 hover:bg-dark-700 rounded-lg transition-colors"
                  >
                    <span className="text-2xl">×</span>
                  </button>
                </div>
                <div className="p-4">
                  <NodePanel node={selectedNode} onClose={handleClosePanel} />
                </div>
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
