document.addEventListener("DOMContentLoaded", () => {
    const chatForm = document.getElementById("chat-form");
    const userInput = document.getElementById("user-input");
    const chatBox = document.getElementById("chat-box");
    
    const uploadBtn = document.getElementById("upload-btn");
    const pdfUpload = document.getElementById("pdf-upload");
    const uploadStatus = document.getElementById("upload-status");
    
    const seedBtn = document.getElementById("seed-btn");
    const seedStatus = document.getElementById("seed-status");

    marked.setOptions({ breaks: true });

    function appendMessage(text, isUser = false) {
        const msgDiv = document.createElement("div");
        msgDiv.className = `message ${isUser ? 'user-message' : 'ai-message'}`;
        
        const avatar = document.createElement("div");
        avatar.className = "avatar";
        avatar.textContent = isUser ? "U" : "AI";
        
        const content = document.createElement("div");
        content.className = "content";
        
        if (isUser) {
            content.textContent = text;
        } else {
            content.innerHTML = marked.parse(text);
        }

        msgDiv.appendChild(avatar);
        msgDiv.appendChild(content);
        chatBox.appendChild(msgDiv);
        
        // Scroll to bottom
        chatBox.scrollTop = chatBox.scrollHeight;
    }

    function showTypingIndicator() {
        const id = 'typing-' + Date.now();
        const msgDiv = document.createElement("div");
        msgDiv.className = "message ai-message";
        msgDiv.id = id;
        
        msgDiv.innerHTML = `
            <div class="avatar">AI</div>
            <div class="content">
                <div class="typing-indicator">
                    <div class="dot"></div>
                    <div class="dot"></div>
                    <div class="dot"></div>
                </div>
            </div>
        `;
        chatBox.appendChild(msgDiv);
        chatBox.scrollTop = chatBox.scrollHeight;
        return id;
    }

    function removeTypingIndicator(id) {
        const el = document.getElementById(id);
        if (el) {
            el.remove();
        }
    }

    chatForm.addEventListener("submit", async (e) => {
        e.preventDefault();
        const text = userInput.value.trim();
        if (!text) return;

        appendMessage(text, true);
        userInput.value = "";
        
        const typingId = showTypingIndicator();

        try {
            const response = await fetch("/api/chat", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ message: text })
            });
            
            removeTypingIndicator(typingId);
            
            if (!response.ok) throw new Error("API request failed");
            
            const data = await response.json();
            appendMessage(data.response);
            
        } catch (error) {
            removeTypingIndicator(typingId);
            appendMessage("**Error:** Could not connect to the Agent API.");
            console.error(error);
        }
    });

    uploadBtn.addEventListener("click", async () => {
        const file = pdfUpload.files[0];
        if (!file) {
            uploadStatus.textContent = "Please select a file first.";
            uploadStatus.className = "error";
            return;
        }

        const formData = new FormData();
        formData.append("file", file);

        uploadBtn.textContent = "Uploading...";
        uploadBtn.disabled = true;

        try {
            const response = await fetch("/api/upload", {
                method: "POST",
                body: formData
            });
            
            const data = await response.json();
            
            if (response.ok) {
                uploadStatus.textContent = "Success!";
                uploadStatus.className = "success";
                appendMessage(`*System:* Uploaded document **${file.name}** to Knowledge Base. You can now talk to it.`);
            } else {
                throw new Error(data.detail || "Upload failed");
            }
        } catch (error) {
            uploadStatus.textContent = error.message;
            uploadStatus.className = "error";
        } finally {
            uploadBtn.textContent = "Upload Document";
            uploadBtn.disabled = false;
            pdfUpload.value = ""; // clear input
        }
    });

    seedBtn.addEventListener("click", async () => {
        seedBtn.textContent = "Seeding...";
        seedBtn.disabled = true;

        try {
            const response = await fetch("/api/seed_recommendations", { method: "POST" });
            const data = await response.json();
            
            if (response.ok) {
                seedStatus.textContent = "Success!";
                seedStatus.className = "success";
                appendMessage("*System:* Initialized recommendation engine with sample product catalog.");
            } else {
                throw new Error("Failed to seed");
            }
        } catch (error) {
            seedStatus.textContent = "Error occurred";
            seedStatus.className = "error";
        } finally {
            seedBtn.textContent = "Seed Catalog";
            seedBtn.disabled = false;
        }
    });
});
