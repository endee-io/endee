"use client";

import React, { useState, useRef, useEffect } from "react";
import { ArrowUp, User, Sparkles } from "lucide-react";
import ReactMarkdown from "react-markdown";

type Message = {
  role: "user" | "assistant" | "system";
  content: string;
};

export default function ChatWindow() {
  const [messages, setMessages] = useState<Message[]>([
    { role: "assistant", content: "Hi! I'm ready to answer questions based on the documents you've ingested into **Endee DB**." }
  ]);
  const [input, setInput] = useState("");
  const [isReceiving, setIsReceiving] = useState(false);
  const endOfMessagesRef = useRef<HTMLDivElement>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  useEffect(() => {
    endOfMessagesRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [messages, isReceiving]);

  const handleInput = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    setInput(e.target.value);
    // Auto-resize textarea
    if (textareaRef.current) {
      textareaRef.current.style.height = "auto";
      textareaRef.current.style.height = `${Math.min(textareaRef.current.scrollHeight, 200)}px`;
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSubmit(e as any);
    }
  };

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!input.trim() || isReceiving) return;

    const userMessage: Message = { role: "user", content: input.trim() };
    setMessages((prev) => [...prev, userMessage]);
    setInput("");
    setIsReceiving(true);
    
    if (textareaRef.current) {
        textareaRef.current.style.height = "auto";
    }

    setMessages((prev) => [...prev, { role: "assistant", content: "" }]);

    try {
      const response = await fetch("/api/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          messages: [...messages, userMessage],
          indexName: "copilot_docs"
        }),
      });

      if (!response.ok) throw new Error("Failed response");

      const reader = response.body?.getReader();
      const decoder = new TextDecoder();
      
      if (reader) {
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          const chunk = decoder.decode(value, { stream: true });
          const lines = chunk.split("\n").filter(line => line.trim() !== "");
          for (const line of lines) {
            const message = line.replace(/^data: /, "");
            if (message === "[DONE]") break;
            try {
              const parsed = JSON.parse(message);
              const content = parsed.choices[0]?.delta?.content || "";
              if (content) {
                setMessages(prev => {
                  const newMsgs = [...prev];
                  const lastMsg = newMsgs[newMsgs.length - 1];
                  if (lastMsg.role === "assistant") lastMsg.content += content;
                  return newMsgs;
                });
              }
            } catch (err) {}
          }
        }
      }
    } catch (err) {
      setMessages(prev => {
        const newMsgs = [...prev];
        const lastMsg = newMsgs[newMsgs.length - 1];
        lastMsg.content += "\n\n**Error:** Cannot connect to Copilot.";
        return newMsgs;
      });
    } finally {
      setIsReceiving(false);
    }
  };

  return (
    <div className="chat-container glass-panel">
      <div className="chat-history">
        {messages.map((msg, i) => (
          <div key={i} className={`message ${msg.role}`}>
            <div className={`avatar ${msg.role}`}>
              {msg.role === "user" ? <User size={20} strokeWidth={2.5} /> : <Sparkles size={20} strokeWidth={2.5} />}
            </div>
            <div className="bubble">
              {msg.role === "assistant" ? (
                <ReactMarkdown>{msg.content}</ReactMarkdown>
              ) : (
                <p>{msg.content}</p>
              )}
            </div>
          </div>
        ))}
        {isReceiving && messages[messages.length - 1]?.content === "" && (
           <div className="message assistant">
             <div className="avatar assistant"><Sparkles size={20} /></div>
             <div className="bubble">
               <div className="typing-indicator">
                 <div className="typing-dot"></div>
                 <div className="typing-dot"></div>
                 <div className="typing-dot"></div>
               </div>
             </div>
           </div>
        )}
        <div ref={endOfMessagesRef} />
      </div>

      <div className="input-section">
        <form onSubmit={handleSubmit} className="input-wrapper">
          <textarea
            ref={textareaRef}
            className="chat-input"
            value={input}
            onChange={handleInput}
            onKeyDown={handleKeyDown}
            placeholder="Ask EndeeCopilot..."
            disabled={isReceiving}
            rows={1}
          />
          <button type="submit" className="send-btn" disabled={!input.trim() || isReceiving}>
            <ArrowUp size={22} strokeWidth={3} />
          </button>
        </form>
      </div>
    </div>
  );
}
