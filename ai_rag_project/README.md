# AI Vector Search Project using Endee Concept

## Overview
This project demonstrates a simple AI system using vector search.

It converts text into embeddings (vectors), stores them, and retrieves the most relevant data based on a user query.

## Features
- Text to vector (embeddings)
- Vector similarity search
- Simple question-answer system

## How it Works
User Query → Convert to Vector → Compare with Stored Vectors → Return Most Relevant Text

## Setup

### 1. Install dependencies
pip install -r requirements.txt

### 2. Store data
python ingest.py

### 3. Run the project
python main.py

## Example

Question:
What is Endee?

Answer:
Endee is a vector database designed for AI applications.
It helps store embeddings and perform similarity search.

## Tech Used
- Python
- Sentence Transformers
- NumPy

## Note
This project demonstrates the concept of vector databases (like Endee) using embeddings and similarity search.