# Agentic Memory in Box

Box provides a dedicated `BoxMemory` layer designed for long-term observation storage and context recall in AI agents.

## Why Agentic Memory?
Unlike standard RAG which only looks at static documentation, Agentic Memory tracks a "stream of consciousness" or tool execution history, enabling:
- **Consistency**: Agents remember past decisions and outcomes.
- **Contextual Recall**: Hybrid search allows finding specific technical details (Sparse) alongside general concepts (Dense).

## Usage

### 1. Store an Observation
You can store any string as a memory.
```python
from box.intelligence import BoxMemory

memory = BoxMemory()
memory.remember("The user preferred HNSW over IVF for this specific index.", {"priority": "high"})
```

### 2. Semantic & Hybrid Recall
Search through memory using natural language.
```python
past_context = memory.recall("What index type did the user want?")
# Result: ["The user preferred HNSW over IVF..."]
```

## IDE Integration
If you are using the **Box VSCode Extension**, the sidebar chat automatically queries your history to provide personalized development suggestions.

## Filtered Memory
You can narrow down recall using metadata filters:
```python
memory.recall("Search logic", filter_dict={"priority": "high"})
```
