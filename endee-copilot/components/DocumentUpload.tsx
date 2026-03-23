"use client";

import React, { useState, useRef } from 'react';
import { Database, FileUp, CheckCircle, AlertTriangle, Loader2 } from 'lucide-react';

export default function DocumentUpload() {
  const [isDragActive, setIsDragActive] = useState(false);
  const [isUploading, setIsUploading] = useState(false);
  const [status, setStatus] = useState<{ type: 'success' | 'error'; message: string } | null>(null);
  
  const fileInputRef = useRef<HTMLInputElement>(null);

  const handleDrag = (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    if (e.type === 'dragenter' || e.type === 'dragover') setIsDragActive(true);
    else if (e.type === 'dragleave') setIsDragActive(false);
  };

  const handleDrop = async (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    setIsDragActive(false);
    if (e.dataTransfer.files && e.dataTransfer.files[0]) {
      await handleFileUpload(e.dataTransfer.files[0]);
    }
  };

  const handleChange = async (e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.files && e.target.files[0]) {
      await handleFileUpload(e.target.files[0]);
    }
  };

  const handleFileUpload = async (file: File) => {
    setIsUploading(true);
    setStatus(null);
    const formData = new FormData();
    formData.append('file', file);
    formData.append('indexName', 'copilot_docs');
    
    try {
      const res = await fetch('/api/ingest', { method: 'POST', body: formData });
      const data = await res.json();
      if (res.ok) setStatus({ type: 'success', message: data.message });
      else setStatus({ type: 'error', message: data.error || 'Upload failed' });
    } catch (err: any) {
      setStatus({ type: 'error', message: err.message || 'Error uploading' });
    } finally {
      setIsUploading(false);
      if (fileInputRef.current) fileInputRef.current.value = '';
    }
  };

  return (
    <div className="sidebar glass-panel">
      <div className="title-container">
        <h1 className="title">Endee<span style={{color: '#fff', fontWeight: 300}}>Copilot</span></h1>
        <p className="subtitle">High-performance AI memory powered by the local Endee Vector DB.</p>
      </div>

      <h3 style={{fontSize: '1.1rem', marginBottom: '1rem', fontWeight: 500, display: 'flex', alignItems: 'center', gap: '0.5rem', color: '#fff'}}>
        <Database size={18} color="var(--accent-cyan)" /> Data Ingestion
      </h3>
      
      <div 
        className={`upload-zone ${isDragActive ? 'active' : ''}`}
        onDragEnter={handleDrag}
        onDragOver={handleDrag}
        onDragLeave={handleDrag}
        onDrop={handleDrop}
        onClick={() => !isUploading && fileInputRef.current?.click()}
        style={{ pointerEvents: isUploading ? 'none' : 'auto', opacity: isUploading ? 0.7 : 1 }}
      >
        <div className="upload-icon-wrapper">
          {isUploading ? (
            <Loader2 size={32} className="upload-icon pulse" style={{color: 'var(--accent-cyan)'}} />
          ) : (
            <FileUp size={32} className="upload-icon" />
          )}
        </div>
        
        <div className="upload-text">
          {isUploading ? (
            "Ingesting into DB..."
          ) : isDragActive ? (
            <span style={{color: 'var(--accent-cyan)', textDecoration: 'none'}}>Drop document here</span>
          ) : (
            <>Drag a document or <span>browse</span></>
          )}
        </div>
        
        <input 
          type="file" 
          ref={fileInputRef} 
          onChange={handleChange} 
          style={{ display: 'none' }} 
          accept=".txt,.md,.csv,.json"
        />
      </div>
      
      {status && (
        <div className={`status-msg ${status.type}`}>
          {status.type === 'success' ? <CheckCircle size={18} /> : <AlertTriangle size={18} />}
          <span>{status.message}</span>
        </div>
      )}
      
      <div style={{marginTop: 'auto', textAlign: 'center', opacity: 0.3, fontSize: '0.8rem', letterSpacing: '0.05em', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '0.4rem'}}>
        <div style={{width: '6px', height: '6px', borderRadius: '50%', background: '#2ed573', boxShadow: '0 0 10px #2ed573'}}></div>
        ENDEE DB CONNECTED
      </div>
    </div>
  );
}
