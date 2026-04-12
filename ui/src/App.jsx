import React, { useState, useEffect } from 'react'
import { motion } from 'framer-motion'
import { Activity, Shield, Terminal, Zap, Github, Settings } from 'lucide-react'
import axios from 'axios'
import ChatBox from './components/ChatBox'
import TaskList from './components/TaskList'

const API_BASE = 'http://localhost:8000'

function App() {
  const [health, setHealth] = useState({ status: 'offline' });

  useEffect(() => {
    const checkHealth = async () => {
      try {
        const res = await axios.get(`${API_BASE}/health`);
        setHealth(res.data);
      } catch {
        setHealth({ status: 'offline' });
      }
    };
    checkHealth();
    const interval = setInterval(checkHealth, 10000);
    return () => clearInterval(interval);
  }, []);

  return (
    <div className="min-h-screen w-screen bg-vibe-bg text-vibe-text font-sans overflow-hidden">
      {/* Background Decor */}
      <div className="fixed top-0 left-0 w-full h-full pointer-events-none">
        <div className="absolute top-[-10%] left-[-10%] w-[40%] h-[40%] bg-vibe-accent/10 rounded-full blur-[120px]" />
        <div className="absolute bottom-[-10%] right-[-10%] w-[30%] h-[30%] bg-purple-500/5 rounded-full blur-[100px]" />
      </div>

      {/* Main Container */}
      <div className="relative z-10 flex h-screen">
        
        {/* Sidebar Navigation */}
        <aside className="w-20 border-r border-white/5 bg-vibe-card/50 backdrop-blur-md flex flex-col items-center py-8 gap-10">
          <div className="w-12 h-12 bg-white/5 rounded-2xl flex items-center justify-center p-1 border border-white/5 shadow-xl">
            <img src="/logo.png" alt="Box Logo" className="w-full h-full object-contain" />
          </div>
          <div className="flex flex-col gap-6 text-slate-500">
            <Activity className="hover:text-vibe-accent cursor-pointer transition-colors" />
            <Terminal className="hover:text-vibe-accent cursor-pointer transition-colors" />
            <Shield className="hover:text-vibe-accent cursor-pointer transition-colors" />
            <Settings className="hover:text-vibe-accent cursor-pointer transition-colors" />
          </div>
          <div className="mt-auto">
             <Github size={20} className="text-slate-500 hover:text-white cursor-pointer" />
          </div>
        </aside>

        {/* Dashboard Content */}
        <main className="flex-1 p-10 flex flex-col gap-10 overflow-y-auto">
          
          {/* Header */}
          <header className="flex justify-between items-end">
            <div>
              <motion.h1 
                initial={{ y: -20, opacity: 0 }}
                animate={{ y: 0, opacity: 1 }}
                className="text-4xl font-extrabold tracking-tight"
              >
                Box <span className="text-vibe-accent">Vibe</span>
              </motion.h1>
              <p className="text-slate-400 mt-2">Autonomous Intelligence • Powered by <span className="text-vibe-accent font-semibold">Endee</span></p>
            </div>
            
            <div className="flex gap-4">
              <div className="px-4 py-2 bg-slate-800/50 rounded-xl border border-white/5 flex items-center gap-3">
                <div className={`w-2 h-2 rounded-full ${health.status === 'ok' ? 'bg-emerald-400 shadow-[0_0_8px_rgba(52,211,153,0.5)]' : 'bg-rose-400'}`} />
                <span className="text-sm font-medium">{health.status === 'ok' ? 'System Online' : 'System Offline'}</span>
              </div>
            </div>
          </header>

          {/* Grid Layout */}
          <div className="grid grid-cols-12 gap-8 flex-1">
            
            {/* Left: Task List (Agentic Memory Core) */}
            <div className="col-span-12 lg:col-span-5 bg-vibe-card/30 backdrop-blur-sm border border-white/5 rounded-3xl p-8 overflow-y-auto max-h-[70vh]">
              <TaskList />
            </div>

            {/* Right: Quick Stats & Insights */}
            <div className="col-span-12 lg:col-span-7 flex flex-col gap-8">
              <div className="p-8 bg-gradient-to-br from-vibe-accent to-blue-700 rounded-3xl text-white shadow-2xl shadow-vibe-accent/10">
                <h3 className="text-lg font-bold opacity-80 uppercase tracking-widest text-sm">Active Repository</h3>
                <div className="text-3xl font-bold mt-4">endee / autonomous-engine</div>
                <div className="mt-6 flex gap-6">
                  <div>
                    <div className="text-2xl font-bold">1.2k</div>
                    <div className="text-xs opacity-60">Knowledge Chunks</div>
                  </div>
                  <div>
                    <div className="text-2xl font-bold">99.4%</div>
                    <div className="text-xs opacity-60">Retrieval Accuracy</div>
                  </div>
                </div>
              </div>

              <div className="grid grid-cols-2 gap-8">
                <div className="p-6 bg-vibe-card/40 border border-white/5 rounded-3xl">
                  <div className="text-vibe-accent mb-3"><Terminal size={24} /></div>
                  <div className="text-sm font-bold">Turbo Indexing</div>
                  <div className="text-xs text-slate-500 mt-1">Delta-Sync active</div>
                </div>
                <div className="p-6 bg-vibe-card/40 border border-white/5 rounded-3xl">
                  <div className="text-emerald-400 mb-3"><Shield size={24} /></div>
                  <div className="text-sm font-bold">Local Priority</div>
                  <div className="text-xs text-slate-500 mt-1">Privacy Secured</div>
                </div>
              </div>
            </div>

          </div>

        </main>

        {/* The Mini Chat Box (🛸 Antigravity Inspiration) */}
        <ChatBox />

      </div>
    </div>
  )
}

export default App
