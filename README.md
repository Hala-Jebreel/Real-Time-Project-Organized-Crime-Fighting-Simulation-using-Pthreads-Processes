# 🕵️ Organized Crime-Fighting Simulation (Project 3)

## 📌 Overview

This project simulates the ongoing battle between **secret agents** and **organized crime gangs** using a combination of **multi-threading (POSIX threads)** and **multi-processing**. It was developed for **ENCS4330: Real-Time Applications & Embedded Systems** at **Birzeit University**.

Gangs commit criminal activities like drug trafficking, robbery, and kidnapping. Meanwhile, secret agents attempt to rise through the gang ranks, gather intelligence, and notify the police without being caught. The simulation tracks promotions, agent exposure, police interventions, and dynamic mission success/failure.

---

## 🔍 Simulation Highlights

- 🦹 **Multiple gangs**, each with configurable number of members and ranks
- 📈 Gang members **get promoted** with time and service
- 🎯 Gang leaders choose targets and assign preparation levels
- 🕵️ Secret agents:
  - Act like gang members
  - Increase knowledge through interaction
  - Report to police based on suspicion level
- 🚔 Police actions:
  - Arrest gang members if multiple agents confirm a threat
  - Maintain suspicion/confidence thresholds
- 🔁 Gangs may **execute**, **fail**, or **uncover infiltrators**
- ⚰️ Gang members may be **killed** during missions and replaced in future rounds
- 🧠 False information and limited visibility per rank simulate real-world complexity

---

## 📂 Project Structure

.
├── src/ # All C source files for gangs, agents, and police
├── config.txt # All runtime parameters
├── include/ # Header files
├── gui/ # Optional OpenGL visualization
├── Makefile # Build script
├── README.md # This file
└── project3_pthread_202502.pdf # Project description



---

## ⚙️ Configuration

User-defined values are stored in `config.txt`, including:

- Number of gangs and members
- Promotion rate
- Mission preparation time
- Agent suspicion threshold
- False info rate
- Prison duration, mission success/failure rate
- Agent death rate
- Simulation end conditions

Example snippet:

```txt
NUM_GANGS=3
MEMBERS_PER_GANG=5
MAX_RANK=4
AGENT_SUCCESS_RATE=70
FALSE_INFO_PROBABILITY=30
SUSPICION_THRESHOLD=60
MAX_SUCCESSFUL_MISSIONS=10
MAX_THWARTED_MISSIONS=8
MAX_EXECUTED_AGENTS=5
