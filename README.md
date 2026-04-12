
## 📦 Box: The Autonomous AI Engine

**Box** is the unified intelligence layer shipping with Endee. It transforms static vector storage into an active developer environment, allowing for high-level repository analysis and autonomous code developments.

### ✨ Key Capabilities

| Feature | Description |
| :--- | :--- |
| **Hybrid Retrieval** | Combines **Dense** embeddings (L6) with **Sparse** (BM25) search for unmatched precision and recall. |
| **Agentic Memory** | Native support for long-term AI memory, enabling agents to retain context across development sessions. |
| **Payload Filtering** | Professional MongoDB-style filtering for Numeric, Category, and Boolean metadata types. |
| **IDE Connectivity** | First-class VSCode sidebar integration and a background REST API for "Kilo Code" style developments. |
| **Operational Control** | Full suite of CLI tools for index health, automated backups, and real-time log monitoring. |

## 🏗️ Architecture

```text
┌──────────────────────────────────────────────────────────┐
│              Autonomous Developer Interface               │
│   ┌──────────────┐      ┌──────────────┐      ┌───────┐  │
│   │ CLI (box)    │ <──> │ VSCode Side  │ <──> │ API   │  │
│   └──────┬───────┘      └──────┬───────┘      └───┬───┘  │
└──────────│─────────────────────│──────────────────│──────┘
           │                     │                  │
           └─────────────────────┴──────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────┐
│                 Box API Server (Port 8000)               │
│      (FastAPI, DeveloperAgent, Contextual Memory)        │
└────────────────────────┬─────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────┐
│              Endee Vector Database (Port 8080)           │
│   ┌─────────────┐     ┌─────────────┐     ┌──────────┐   │
│   │ Dense Index │     │ Sparse Index│     │ Filters  │   │
│   └─────────────┘     └─────────────┘     └──────────┘   │
└──────────────────────────────────────────────────────────┘
```

---

## 🚀 Getting Started

### 1 — One-Click Setup (Master Suite)
Initialize the entire environment (Dependencies, PATH, and Initial Indexing) with a single command:

**Windows:**
```powershell
.\setup.bat
```

**Linux / macOS / BSD:**
```bash
chmod +x setup.sh && ./setup.sh
```

### 2 — Starting the Intelligence Layer
Launch the Box API Server to enable IDE and external tool connectivity:
```bash
box serve  # (Or ./box.sh serve)
```

### 3 — Building the Sidebar (VSCode)
To use the included IDE extension, use our build tool to compile the TypeScript source:

**Windows:**
```powershell
.\ide\vscode\build.bat
```

**Linux / macOS / BSD:**
```bash
chmod +x ide/vscode/build.sh && ./ide/vscode/build.sh
```

---

## 🛠️ Usage Workflow

### 1. Codebase Awareness
Before using the AI, ensuring your index is fresh.
```bash
box index
```

### 2. Autonomous IDE Chat
1. Launch the API: `box serve`
2. Open VSCode and navigate to the **Box Engine** activity bar icon.
3. Start chatting with your codebase! Box uses hybrid semantic search to find relevant context automatically.

### 3. Agentic Development
Use the CLI for direct autonomous improvements:
```bash
box search "logic for vector upsert" --top_k 3
```
Or use the context-aware `develop` API via the VSCode extension commands (`Box: Ask about current file`).

---

## 📂 Project Structure

| Directory | Content |
| :--- | :--- |
| **`box/`** | 🧠 The core Intelligence Engine (CLI, Server, Logic). |
| **`ide/`** | 🧩 IDE extension source code and build tools. |
| **`docs/`** | 📖 Comprehensive technical guides (Memory, Filtering, Sparse). |
| **`data/`** | 📂 Sample datasets for initial evaluation. |
| **`box.bat` / `box.sh`** | 🚀 Unified entry points for Windows and UNIX-like systems. |

## ⚙️ CLI Reference

Use `box` (if installed to PATH) or `./box.sh` / `.\box.bat`:

- `box index` - Scans and indexes the current codebase.
- `box search "query"` - Performs a hybrid semantic lookup.
- `box status` - Real-time system health and connectivity check.
- `box backup create` - Generates a snapshot of your index.

---

## 🏗️ Technical Details
- **Architecture**: Multi-layered (DB <-> API <-> IDE)
- **Platforms**: Windows, Linux (Ubuntu/Debian/RHEL/Fedora), macOS (arm64), FreeBSD/OpenBSD/NetBSD.
- **Dependencies**: Python 3.10+, Node.js (for IDE), C++17 Compiler (for core build).

## 🛡️ License

Endee is open-source software licensed under the **Apache License 2.0**. For enterprise support or branding permissions, contact [enterprise@endee.io](mailto:enterprise@endee.io).

---
<p align="center">
    <b>Built with performance in mind for all major operating systems.</b>
</p>
