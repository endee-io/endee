# Nexus AI: Multimodal Semantic Search Engine

  

An internship project built for the Endee AI Challenge. This application performs high-accuracy semantic image retrieval through a Telegram interface.

  

## 🏗️ System Design (Adaptive Retrieval Architecture)

  

My implementation follows a production-style AI pipeline designed for low-latency search:

  

1.  **Preprocessing Layer:** Images stored in `/pics` are loaded via Pillow and normalized for the AI model.

2.  **Embedding Engine:** Uses the `CLIP (ViT-B-32)` transformer model. This is a **multimodal** model that understands the relationship between natural language and visual pixels, converting them into 512-dimensional vectors.

3.  **Local Vector Database:** To ensure portability and overcome environment constraints with Docker, I developed a custom **JSON-based Vector Store** (`vector_db.json`). This stores the file paths and their corresponding high-dimensional embeddings.

4.  **Search & Similarity Logic:**

- The system calculates the **Cosine Similarity** between the user's text query and all stored image vectors.

-  **Adaptive Filtering:** I implemented a custom "Smart Gate." If the top match is significantly stronger than the rest, only 1 result is sent. If multiple images are highly relevant (within a 90% similarity threshold), the bot adaptively returns the top 2.

  

## 🛠️ How I Built It

-  **Semantic Search:** Unlike keyword search, this bot understands concepts. Searching "mammal" will find a "bear" even without that specific word in the filename.

-  **Python-Telegram-Bot:** A clean, real-world interface for user interaction.

-  **NumPy Math:** Used for fast vector normalization and dot-product calculations.

  

## 🚀 Setup Instructions

  

### 1. Install Dependencies

Run the following command to install the required AI libraries:

```bash
pip  install  sentence-transformers  pillow  python-telegram-bot  numpy
```
### 2.  Index  Your  Images

Ensure  your  images  are  in  the  /pics  folder,  then  run  the  engine  to  generate  your  vector  database:
```
python  engine.py
```
  
  ### 3.  Start  the  Bot

Update  the  token  in  bot.py  and  run  the  main  service  to  go  live:
```  
python  bot.py
```  

# 📝  Project  Evolution  Note

Originally  designed  for  the  Endee  Docker  environment,  I  successfully  pivoted  to  a  Local  Vector  Engine  implementation  to  ensure  100%  functionality  and  easier  evaluation  for  the  technical  team  during  this  24-hour  challenge.