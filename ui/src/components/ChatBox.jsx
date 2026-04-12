import React, { useState, useEffect, useRef } from 'react';
import { motion, AnimatePresence } from 'framer-motion';
import { Send, Bot, User, X, Maximize2, Minimize2 } from 'lucide-react';
import axios from 'axios';

const API_BASE = 'http://localhost:8000';

const ChatBox = () => {
    const [messages, setMessages] = useState([
        { role: 'bot', text: 'Hey! I\'m your Box Intelligence Agent. How can I help with the codebase?' }
    ]);
    const [input, setInput] = useState('');
    const [isExpanded, setIsExpanded] = useState(true);
    const [loading, setLoading] = useState(false);
    const chatEndRef = useRef(null);

    const scrollToBottom = () => {
        chatEndRef.current?.scrollIntoView({ behavior: "smooth" });
    };

    useEffect(() => {
        scrollToBottom();
    }, [messages]);

    const handleSend = async () => {
        if (!input.trim()) return;
        
        const userMsg = input;
        setInput('');
        setMessages(prev => [...prev, { role: 'user', text: userMsg }]);
        setLoading(true);

        try {
            const res = await axios.post(`${API_BASE}/chat`, { query: userMsg });
            setMessages(prev => [...prev, { role: 'bot', text: res.data.response }]);
        } catch (err) {
            setMessages(prev => [...prev, { role: 'bot', text: "Error: Couldn't reach the intelligence server." }]);
        } finally {
            setLoading(false);
        }
    };

    return (
        <div className="fixed bottom-6 right-6 z-50">
            <AnimatePresence>
                {isExpanded ? (
                    <motion.div 
                        initial={{ opacity: 0, scale: 0.9, y: 20 }}
                        animate={{ opacity: 1, scale: 1, y: 0 }}
                        exit={{ opacity: 0, scale: 0.9, y: 20 }}
                        className="w-96 h-[500px] bg-vibe-card/80 backdrop-blur-xl border border-white/10 rounded-2xl shadow-2xl flex flex-col overflow-hidden"
                    >
                        {/* Header */}
                        <div className="p-4 bg-vibe-accent/10 border-b border-white/5 flex justify-between items-center">
                            <div className="flex items-center gap-2">
                                <div className="p-1.5 bg-vibe-accent rounded-lg">
                                    <Bot size={18} className="text-white" />
                                </div>
                                <span className="font-semibold text-white tracking-tight">Box Intelligence</span>
                            </div>
                            <button onClick={() => setIsExpanded(false)} className="text-slate-400 hover:text-white transition-colors">
                                <Minimize2 size={18} />
                            </button>
                        </div>

                        {/* Messages */}
                        <div className="flex-1 overflow-y-auto p-4 space-y-4">
                            {messages.map((msg, i) => (
                                <div key={i} className={`flex ${msg.role === 'user' ? 'justify-end' : 'justify-start'}`}>
                                    <div className={`max-w-[80%] p-3 rounded-2xl text-sm ${msg.role === 'user' ? 'bg-vibe-accent text-white rounded-tr-none' : 'bg-slate-800 text-slate-100 rounded-tl-none border border-white/5'}`}>
                                        {msg.text}
                                    </div>
                                </div>
                            ))}
                            {loading && (
                                <div className="flex justify-start">
                                    <div className="bg-slate-800 p-3 rounded-2xl rounded-tl-none border border-white/5 flex gap-1">
                                        <motion.div animate={{ opacity: [0, 1, 0] }} transition={{ repeat: Infinity, duration: 1 }} className="w-1.5 h-1.5 bg-slate-400 rounded-full" />
                                        <motion.div animate={{ opacity: [0, 1, 0] }} transition={{ repeat: Infinity, duration: 1, delay: 0.2 }} className="w-1.5 h-1.5 bg-slate-400 rounded-full" />
                                        <motion.div animate={{ opacity: [0, 1, 0] }} transition={{ repeat: Infinity, duration: 1, delay: 0.4 }} className="w-1.5 h-1.5 bg-slate-400 rounded-full" />
                                    </div>
                                </div>
                            )}
                            <div ref={chatEndRef} />
                        </div>

                        {/* Input */}
                        <div className="p-4 bg-vibe-bg/50 border-t border-white/5">
                            <div className="relative flex items-center">
                                <input 
                                    type="text" 
                                    value={input}
                                    onChange={(e) => setInput(e.target.value)}
                                    onKeyDown={(e) => e.key === 'Enter' && handleSend()}
                                    placeholder="Ask about your code..."
                                    className="w-full bg-slate-900 border border-white/10 rounded-xl py-3 pl-4 pr-12 text-sm text-white focus:outline-none focus:border-vibe-accent transition-all shadow-inner"
                                />
                                <button 
                                    onClick={handleSend}
                                    className="absolute right-2 p-2 text-vibe-accent hover:bg-vibe-accent hover:text-white rounded-lg transition-all"
                                >
                                    <Send size={18} />
                                </button>
                            </div>
                        </div>
                    </motion.div>
                ) : (
                    <motion.button
                        initial={{ scale: 0 }}
                        animate={{ scale: 1 }}
                        whileHover={{ scale: 1.1 }}
                        whileTap={{ scale: 0.9 }}
                        onClick={() => setIsExpanded(true)}
                        className="w-14 h-14 bg-vibe-accent rounded-full flex items-center justify-center shadow-xl shadow-vibe-accent/20 text-white"
                    >
                        <Bot size={24} />
                    </motion.button>
                )}
            </AnimatePresence>
        </div>
    );
};

export default ChatBox;
