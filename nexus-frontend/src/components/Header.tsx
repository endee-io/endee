'use client'

import { Brain, Menu, X } from 'lucide-react'

interface HeaderProps {
  onToggleControlPanel: () => void
  showControlPanel: boolean
}

export default function Header({ onToggleControlPanel, showControlPanel }: HeaderProps) {
  return (
    <header className="bg-dark-800 border-b border-dark-700 px-4 sm:px-6 py-4">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2 sm:gap-3 flex-1">
          <div className="w-10 h-10 bg-gradient-to-br from-primary-500 to-primary-700 rounded-lg flex items-center justify-center flex-shrink-0">
            <Brain className="w-6 h-6 text-white" />
          </div>
          <div className="min-w-0">
            <h1 className="text-lg sm:text-2xl font-bold bg-gradient-to-r from-primary-400 to-primary-600 bg-clip-text text-transparent truncate">
              Nexus
            </h1>
            <p className="text-xs text-dark-400 hidden sm:block">Self-Evolving AI Knowledge Network</p>
          </div>
        </div>

        <div className="flex items-center gap-4 ml-4">
          {/* Status Indicator - Hidden on mobile */}
          <div className="hidden md:flex items-center gap-2 text-sm text-dark-400">
            <div className="w-2 h-2 rounded-full bg-green-500 animate-pulse"></div>
            <span className="hidden lg:inline">Vector-native intelligence</span>
          </div>

          {/* Mobile Menu Toggle */}
          <button
            onClick={onToggleControlPanel}
            className="md:hidden p-2 hover:bg-dark-700 rounded-lg transition-colors"
            aria-label="Toggle controls"
          >
            {showControlPanel ? (
              <X className="w-5 h-5 text-dark-400" />
            ) : (
              <Menu className="w-5 h-5 text-dark-400" />
            )}
          </button>
        </div>
      </div>
    </header>
  )
}
