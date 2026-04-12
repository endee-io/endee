import React, { useState, useEffect } from 'react';
import { motion } from 'framer-motion';
import { CheckCircle2, Circle, Clock, AlertCircle } from 'lucide-react';
import axios from 'axios';

const API_BASE = 'http://localhost:8000';

const TaskList = () => {
    const [tasks, setTasks] = useState([]);

    useEffect(() => {
        const fetchTasks = async () => {
            try {
                const res = await axios.get(`${API_BASE}/tasks`);
                setTasks(res.data.tasks);
            } catch (err) {
                console.error("Failed to fetch tasks");
            }
        };
        fetchTasks();
        const interval = setInterval(fetchTasks, 5000);
        return () => clearInterval(interval);
    }, []);

    const getStatusIcon = (status) => {
        switch (status) {
            case 'completed': return <CheckCircle2 className="text-emerald-400" size={18} />;
            case 'in_progress': return <Clock className="text-vibe-accent animate-pulse" size={18} />;
            case 'failed': return <AlertCircle className="text-rose-400" size={18} />;
            default: return <Circle className="text-slate-500" size={18} />;
        }
    };

    return (
        <div className="space-y-4">
            <h2 className="text-xl font-bold text-white mb-6">Autonomous Goals</h2>
            {tasks.length === 0 ? (
                <div className="p-8 text-center bg-white/5 rounded-2xl border border-dashed border-white/10 text-slate-400">
                    No active tasks. Start a vibe to see goals!
                </div>
            ) : (
                tasks.map((task, i) => (
                    <motion.div 
                        key={task.id}
                        initial={{ x: -20, opacity: 0 }}
                        animate={{ x: 0, opacity: 1 }}
                        transition={{ delay: i * 0.1 }}
                        className="p-4 bg-slate-800/40 border border-white/5 rounded-2xl hover:bg-slate-800/60 transition-all flex gap-4 items-start"
                    >
                        <div className="mt-1">{getStatusIcon(task.current_status)}</div>
                        <div className="flex-1">
                            <div className="text-sm font-medium text-white">{task.goal}</div>
                            <div className="text-xs text-slate-500 mt-1 flex items-center gap-2">
                                <span className="uppercase tracking-wider">{task.current_status}</span>
                                <span>•</span>
                                <span>{new Date(task.created_at).toLocaleTimeString()}</span>
                            </div>
                        </div>
                    </motion.div>
                ))
            )}
        </div>
    );
};

export default TaskList;
