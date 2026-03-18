const input = document.getElementById("input");
const messagesDiv = document.getElementById("messages");
const statusBadge = document.getElementById("statusBadge");
const sendBtn = document.getElementById("sendBtn");
const toggleContextBtn = document.getElementById("toggleContextBtn");

const BACKEND_URL = "https://endee-rag-chatbot-1.onrender.com";

let showContext = true;
let isSending = false;

window.addEventListener("load", async() => {
    addWelcomeMessage();
    await wakeUpBackend();
});

async function wakeUpBackend() {
    try {
        if (statusBadge) statusBadge.textContent = "Connecting...";
        addSystemMessage("Connecting to server...");

        const res = await fetch(`${BACKEND_URL}/health`, {
            method: "GET"
        });

        if (!res.ok) {
            throw new Error("Health check failed");
        }

        removeSystemMessage();

        if (statusBadge) statusBadge.textContent = "Online";
        addSystemMessage("Server connected successfully");

        setTimeout(removeSystemMessage, 1500);
    } catch (error) {
        removeSystemMessage();

        if (statusBadge) statusBadge.textContent = "Waking up...";
        addSystemMessage("Server is waking up. First reply may take a few seconds.");

        console.error("Wake-up error:", error);
    }
}

async function send() {
    const msg = input.value.trim();

    if (!msg || isSending) return;

    isSending = true;
    sendBtn.disabled = true;

    addMessage("🧑 " + msg, "user");

    input.value = "";
    input.focus();

    addLoadingMessage("🤖 Thinking...");

    try {
        const res = await fetch(`${BACKEND_URL}/chat`, {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({ message: msg })
        });

        let data = {};
        try {
            data = await res.json();
        } catch (e) {
            data = {};
        }

        removeLoadingMessage();

        if (!res.ok) {
            throw new Error(data.detail || `HTTP ${res.status}: Request failed`);
        }

        if (statusBadge) statusBadge.textContent = "Online";

        addMessage("🤖 " + (data.reply || "No reply received from server."), "bot");

        if (showContext && data.matched_chunks && data.matched_chunks.length > 0) {
            addContextBox(data.matched_chunks);
        }

    } catch (error) {
        removeLoadingMessage();

        if (statusBadge) statusBadge.textContent = "Error";
        addMessage("🤖 Error: " + error.message, "bot");

        console.error("Chat error:", error);
    } finally {
        isSending = false;
        sendBtn.disabled = false;
    }
}

function addWelcomeMessage() {
    addMessage(
        "🤖 Hello 👋\nI am an Endee-powered RAG assistant.\nI retrieve relevant knowledge and generate accurate answers.\nAsk me anything about RAG, vector databases, or this project.",
        "bot"
    );
}

function fillPrompt(text) {
    input.value = text;
    input.focus();
}

async function clearChat() {
    messagesDiv.innerHTML = "";
    addWelcomeMessage();

    try {
        await fetch(`${BACKEND_URL}/clear`, {
            method: "POST"
        });

        if (statusBadge) statusBadge.textContent = "Online";
        addSystemMessage("Chat history cleared successfully");
        setTimeout(removeSystemMessage, 1500);
    } catch (error) {
        console.error("Failed to clear backend chat history:", error);
        addSystemMessage("Frontend chat cleared, but backend memory could not be cleared.");
        setTimeout(removeSystemMessage, 2000);
    }
}

function toggleContextVisibility() {
    showContext = !showContext;

    if (toggleContextBtn) {
        toggleContextBtn.textContent = showContext ? "Hide Context" : "Show Context";
    }
}

function addMessage(text, sender, id = null) {
    const msgDiv = document.createElement("div");
    msgDiv.className = `message ${sender}`;
    msgDiv.textContent = text;

    if (id) {
        msgDiv.id = id;
    }

    messagesDiv.appendChild(msgDiv);
    scrollToBottom();
}

function addLoadingMessage(text) {
    removeLoadingMessage();

    const loadingDiv = document.createElement("div");
    loadingDiv.className = "message bot loading";
    loadingDiv.id = "loading";
    loadingDiv.textContent = text;

    messagesDiv.appendChild(loadingDiv);
    scrollToBottom();
}

function removeLoadingMessage() {
    const loading = document.getElementById("loading");
    if (loading) {
        loading.remove();
    }
}

function addSystemMessage(text) {
    removeSystemMessage();

    const systemDiv = document.createElement("div");
    systemDiv.className = "message system";
    systemDiv.id = "system-message";
    systemDiv.textContent = text;

    messagesDiv.appendChild(systemDiv);
    scrollToBottom();
}

function removeSystemMessage() {
    const systemMsg = document.getElementById("system-message");
    if (systemMsg) {
        systemMsg.remove();
    }
}

function addContextBox(chunks) {
    const contextDiv = document.createElement("div");
    contextDiv.className = "context-box";

    const title = document.createElement("div");
    title.className = "context-title";
    title.textContent = "📌 Retrieved from Knowledge Base";

    contextDiv.appendChild(title);

    chunks.forEach((chunk) => {
        const item = document.createElement("div");
        item.className = "context-item";
        item.textContent = `• ${chunk}`;
        contextDiv.appendChild(item);
    });

    messagesDiv.appendChild(contextDiv);
    scrollToBottom();
}

function scrollToBottom() {
    messagesDiv.scrollTop = messagesDiv.scrollHeight;
}

input.addEventListener("keydown", function(e) {
    if (e.key === "Enter") {
        e.preventDefault();
        send();
    }
});