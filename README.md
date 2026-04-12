# 🚑 RescuFlow AI: Vector-Based Emergency Routing

![Python](https://img.shields.io/badge/Python-3.12-blue?logo=python)
![AI](https://img.shields.io/badge/AI-Vector%20Search-green)
![Database](https://img.shields.io/badge/VectorDB-Endee-black)
![Blockchain](https://img.shields.io/badge/Blockchain-Ethereum-blueviolet?logo=ethereum)
![Frontend](https://img.shields.io/badge/Frontend-Streamlit-red?logo=streamlit)
![Deployment](https://img.shields.io/badge/Deployment-Docker-ready-orange?logo=docker)
![Status](https://img.shields.io/badge/Status-Production--Style-success)

---

##  Project Overview

**RescuFlow AI** is a production-style emergency dispatch system that uses **vector similarity search + blockchain auditing** to optimize emergency routing.

Unlike traditional GPS systems that focus on the *shortest distance*, RescuFlow selects routes based on the **highest probability of success**, considering real-world conditions like:
 Traffic
 Weather
 Construction
 Time of day

---

##  Core Concept

Road conditions are modeled as vectors:

```
V = [Traffic, Construction, Weather, TimeOfDay]
```

The system compares **current conditions** with **historical vectors** to find the most reliable and safest route using **nearest neighbor search**.

---

## 🏗️ System Architecture

### 🔹 Intelligence Layer (Endee)

* Rust-based vector database
* Stores historical road condition vectors
* Performs high-speed similarity search

### 🔹 Trust Layer (Blockchain)

* Built using Ethereum (Ganache)
* Logs every routing decision
* Ensures transparency & tamper-proof auditing

### 🔹 UI Layer (Streamlit)

* Real-time dashboard
* Live route visualization
* System performance monitoring

---

##   Evaluation Mapping

| Requirement                | Implementation (RescuFlow AI)                        | Status      |
| -------------------------- | ---------------------------------------------------- | ----------- |
| Vector Database Usage      | Endee integration for semantic route matching        | ✅ Satisfied |
| Semantic Decision Making   | Route selection via vector similarity (not distance) | ✅ Satisfied |
| Blockchain Integration     | Ethereum-based audit logging (Ganache + Solidity)    | ✅ Satisfied |
| Real-World Problem Solving | Emergency routing optimized for success rate         | ✅ Satisfied |
| System Performance         | Sub-ms vector search + real-time monitoring          | ✅ Satisfied |

---

##  Key Features

 **Semantic Route Optimization**

* Chooses safest route, not shortest

 **Vector-Based Intelligence**

* Uses similarity search on real-world conditions

 **Blockchain Audit Logging**

* Every decision is recorded on-chain

 **Real-Time Dashboard**

* Monitor routes, latency, and system health

 **Fault-Tolerant System**

* Includes fallback routing logic

 **Scalable Architecture**

* Ready for IoT and smart city integration

---

##  Tech Stack

| Category        | Technology                 |
| --------------- | -------------------------- |
| 🧠 AI / Backend | Python, NumPy, REST APIs   |
| 🗄️ Vector DB   | Endee                      |
| 🔗 Blockchain   | Solidity, Web3.py, Ganache |
| 🎨 Frontend     | Streamlit                  |

---

## ⚙️ Installation & Setup

### 1️⃣ Start Endee

```bash id="e1a2b3"
./endee --port 8080
```

### 2️⃣ Setup Blockchain (Optional)

* Start Ganache
* Deploy `BlockVerify.sol` from `/contracts`
* Update contract address in `app.py`

### 3️⃣ Install Dependencies

```bash id="c4d5e6"
pip install streamlit pandas numpy web3 requests
```

### 4️⃣ Run the Application

```bash id="f7g8h9"
streamlit run app.py
```



##  Why Vector Search?

Traditional systems rely on rigid queries and static rules.

**Vector search enables semantic understanding**, allowing the system to:
✔️ Adapt to real-world dynamic conditions
✔️ Learn from historical patterns
✔️ Make smarter, safer routing decisions

---

## 🔄 Workflow (Agentic System)

**Route → Evaluate → Select → Log → Trigger IoT**

*  Route calculated using vector similarity
*  Evaluated against historical success data
*  Logged on blockchain
*  Can trigger IoT devices (signals, alerts)

---

## Screenshots
<img width="1920" height="1080" alt="Screenshot (985)" src="https://github.com/user-attachments/assets/7c605989-57e4-4af5-89cb-a83e3ad9cdc2" />
<img width="1920" height="1080" alt="Screenshot (986)" src="https://github.com/user-attachments/assets/aa6ec210-a379-487c-a9da-9aa32f269995" />



If you like this project:
⭐ Star the repository
🔗 Share with others
🚀 Contribute to improvements

---

🔥 *Built to demonstrate real-world AI + Blockchain + System Design skills for top-tier engineering roles.*
