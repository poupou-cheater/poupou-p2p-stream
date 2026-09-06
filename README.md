<div align="center">

# ⚡ Poupou P2P Stream

### *Ultra Low-Latency, Direct Peer-to-Peer Screen Streaming, Voice Calling & Real-Time Collaborative Annotation Engine*

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=for-the-badge&logo=c%2B%2B)](https://en.cppreference.com/)
[![Dear ImGui](https://img.shields.io/badge/UI-Dear%20ImGui%20v1.92-E65C00?style=for-the-badge&logo=gui)](https://github.com/ocornut/imgui)
[![Tailscale P2P](https://img.shields.io/badge/Network-Tailscale%20%2F%20Direct%20P2P-2D3748?style=for-the-badge&logo=tailscale)](https://tailscale.com)

</div>

## 📖 Overview

**Poupou P2P Stream** is a lightweight, high-performance desktop application designed for private, high-fidelity peer-to-peer screen streaming, crystal-clear voice communication, and interactive real-time screen collaboration.

Unlike traditional conferencing tools that rely on centralized servers and heavy web runtimes, **Poupou P2P Stream** connects peers directly using encrypted mesh networks (such as **Tailscale** or direct IP routing) and renders with bare-metal **DirectX 11** and **Dear ImGui** for sub-frame latency and minimal resource overhead.

---

## 📐 Architectural Blueprint & UI Design

The complete user experience and system architecture are designed around an intuitive, modular wireframe grid:

<div align="center">

![Poupou P2P Stream Architecture & UI Wireframe Blueprint](./ext/img/architectural%20sketche.png)

*Figure 1: Architectural Wireframes & Feature Blueprint for Poupou P2P Stream.*

</div>

---

## 🛠️ Technology Stack

| Layer | Technology | Purpose |
| :--- | :--- | :--- |
| **Language** | **C++20** | High-performance, modern native execution with zero garbage collection |
| **Graphics API** | **DirectX 11 (D3D11)** | Hardware-accelerated surface rendering, swapchain presentation, and texture mapping |
| **UI Framework** | **Dear ImGui (v1.92.6)** | Immediate-mode GUI with custom styling, docking, and flexible viewport controls |
| **OS & Windowing** | **Win32 API / DWM** | Native Windows window management, message dispatching, and DWM composition |
| **Audio Processing** | **RNNoise & WASAPI** | Neural noise suppression and low-latency audio capture/playback |
| **Networking** | **P2P UDP / Tailscale** | Direct encrypted point-to-point data transport |
| **Vector Rendering** | **NanoSVG & DirectX 11** | High-DPI in-memory SVG parsing, rasterization, and GPU SRV texture generation |
| **Assets** | **Modern SVG Icons** | from icons.js.com


---
