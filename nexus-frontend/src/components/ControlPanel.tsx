'use client'

import { useState, useRef, FormEvent } from 'react'
import { useGraphStore } from '@/store/graphStore'
import { Upload, Search, Loader2, AlertCircle, X } from 'lucide-react'

interface ControlPanelProps {
  onClose?: () => void
}

export default function ControlPanel({ onClose }: ControlPanelProps) {
  const [query, setQuery] = useState('')
  const [isUploading, setIsUploading] = useState(false)
  const [uploadError, setUploadError] = useState<string | null>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)
  
  const { executeQuery, uploadDocument, fetchGraph, isLoading, error } = useGraphStore()

  const handleSearch = async (e: FormEvent) => {
    e.preventDefault()
    if (query.trim()) {
      await executeQuery(query)
    }
  }

  const handleFileUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    if (!file) return

    // Validate file type
    const allowedTypes = ['.pdf', '.txt', '.md', '.docx']
    const fileExt = file.name.toLowerCase().match(/\.[^.]+$/)?.[0]
    
    if (!fileExt || !allowedTypes.includes(fileExt)) {
      setUploadError(`Invalid file type. Allowed: ${allowedTypes.join(', ')}`)
      return
    }

    // Validate file size (10MB max)
    if (file.size > 10 * 1024 * 1024) {
      setUploadError('File size must be less than 10MB')
      return
    }

    setIsUploading(true)
    setUploadError(null)

    try {
      await uploadDocument(file)
      // Reset file input
      if (fileInputRef.current) {
        fileInputRef.current.value = ''
      }
    } catch (err: any) {
      setUploadError(err.message || 'Upload failed')
    } finally {
      setIsUploading(false)
    }
  }

  const handleRefresh = () => {
    fetchGraph()
  }

  return (
    <div className="h-full flex flex-col bg-dark-800 md:bg-transparent">
      {/* Header */}
      <div className="border-b border-dark-700 px-4 sm:px-6 py-4 flex-shrink-0">
        <div className="flex items-start justify-between">
          <div className="flex-1 min-w-0">
            <h2 className="text-lg sm:text-xl font-semibold text-white mb-1">Knowledge Network</h2>
            <p className="text-xs sm:text-sm text-dark-400 hidden sm:block">Upload documents and explore</p>
          </div>
          {onClose && (
            <button
              onClick={onClose}
              className="md:hidden p-2 hover:bg-dark-700 rounded-lg transition-colors ml-2 flex-shrink-0"
              aria-label="Close controls"
            >
              <X className="w-5 h-5 text-dark-400" />
            </button>
          )}
        </div>
      </div>

      {/* Content */}
      <div className="flex-1 overflow-y-auto p-4 sm:p-6 space-y-6">
        {/* Error Display */}
      {(error || uploadError) && (
        <div className="bg-red-500/10 border border-red-500/30 rounded-lg p-3 flex items-start gap-2">
          <AlertCircle className="w-5 h-5 text-red-400 flex-shrink-0 mt-0.5" />
          <p className="text-sm text-red-300">{error || uploadError}</p>
        </div>
      )}

      {/* Upload Section */}
      <div className="space-y-3">
        <h3 className="text-sm font-medium text-dark-300">Upload Documents</h3>
        
        <div className="border-2 border-dashed border-dark-600 rounded-lg p-4 sm:p-6 text-center hover:border-primary-500 transition-colors cursor-pointer">
          <input
            ref={fileInputRef}
            type="file"
            onChange={handleFileUpload}
            accept=".pdf,.txt,.md,.docx"
            className="hidden"
            id="file-upload"
            disabled={isUploading}
          />
          <label
            htmlFor="file-upload"
            className={`cursor-pointer flex flex-col items-center gap-2 ${
              isUploading ? 'opacity-50 cursor-not-allowed' : ''
            }`}
          >
            {isUploading ? (
              <Loader2 className="w-8 sm:w-10 h-8 sm:h-10 text-primary-500 animate-spin" />
            ) : (
              <Upload className="w-8 sm:w-10 h-8 sm:h-10 text-dark-400" />
            )}
            <span className="text-xs sm:text-sm text-dark-300">
              {isUploading ? 'Uploading...' : 'Click to upload'}
            </span>
            <span className="text-xs text-dark-500">
              PDF, TXT, MD, DOCX (max 10MB)
            </span>
          </label>
        </div>

        <div className="text-xs text-dark-500 space-y-1">
          <p>✓ Automatic chunking and embedding</p>
          <p>✓ Real-time graph updates</p>
          <p>✓ Semantic relationship discovery</p>
        </div>
      </div>

      {/* Search Section */}
      <div className="space-y-3">
        <h3 className="text-sm font-medium text-dark-300">Semantic Search</h3>
        
        <form onSubmit={handleSearch} className="space-y-2">
          <div className="relative">
            <Search className="absolute left-3 top-1/2 transform -translate-y-1/2 w-4 h-4 text-dark-400 pointer-events-none" />
            <input
              type="text"
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              placeholder="Search graph..."
              className="w-full bg-dark-700 border border-dark-600 rounded-lg pl-10 pr-4 py-2 text-sm text-white placeholder-dark-400 focus:outline-none focus:border-primary-500 transition-colors"
              disabled={isLoading}
            />
          </div>
          
          <button
            type="submit"
            disabled={isLoading || !query.trim()}
            className="w-full bg-primary-600 hover:bg-primary-700 disabled:bg-dark-700 disabled:text-dark-500 text-white rounded-lg py-2 text-sm font-medium transition-colors"
          >
            {isLoading ? (
              <span className="flex items-center justify-center gap-2">
                <Loader2 className="w-4 h-4 animate-spin" />
                <span className="hidden sm:inline">Searching...</span>
                <span className="sm:hidden">Search...</span>
              </span>
            ) : (
              'Search'
            )}
          </button>
        </form>

        <div className="text-xs text-dark-500">
          <p className="mb-1">Example queries:</p>
          <div className="space-y-1">
            <button
              onClick={() => setQuery('machine learning algorithms')}
              className="block w-full text-left px-2 py-1 rounded hover:bg-dark-700 transition-colors"
            >
              • machine learning algorithms
            </button>
            <button
              onClick={() => setQuery('neural network architecture')}
              className="block w-full text-left px-2 py-1 rounded hover:bg-dark-700 transition-colors"
            >
              • neural network architecture
            </button>
            <button
              onClick={() => setQuery('vector embeddings')}
              className="block w-full text-left px-2 py-1 rounded hover:bg-dark-700 transition-colors"
            >
              • vector embeddings
            </button>
          </div>
        </div>
      </div>

      {/* Actions Section */}
      <div className="space-y-3">
        <h3 className="text-sm font-medium text-dark-300">Actions</h3>
        
        <button
          onClick={handleRefresh}
          disabled={isLoading}
          className="w-full bg-dark-700 hover:bg-dark-600 text-white rounded-lg py-2 text-sm font-medium transition-colors disabled:opacity-50"
        >
          Refresh Graph
        </button>
      </div>
      </div>

      {/* Quick Stats */}
      <div className="border-t border-dark-700 pt-4 px-4 sm:px-6 pb-4 flex-shrink-0">
        <h3 className="text-sm font-medium text-dark-300 mb-3">Quick Stats</h3>
        <div className="grid grid-cols-2 gap-3">
          <div className="bg-dark-700 rounded-lg p-3">
            <div className="text-2xl font-bold text-primary-400">--</div>
            <div className="text-xs text-dark-400">Documents</div>
          </div>
          <div className="bg-dark-700 rounded-lg p-3">
            <div className="text-2xl font-bold text-primary-400">--</div>
            <div className="text-xs text-dark-400">Concepts</div>
          </div>
        </div>
      </div>
    </div>
  )
}
