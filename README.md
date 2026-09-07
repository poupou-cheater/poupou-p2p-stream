<div align="center">

# ⚡ Poupou P2P Stream

### *Ultra Low-Latency, Direct Peer-to-Peer Screen Streaming, Voice Calling & Real-Time Collaborative Annotation Engine*

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=for-the-badge&logo=c%2B%2B)](https://en.cppreference.com/)
[![DirectX 11](https://img.shields.io/badge/Graphics-DirectX%2011-107C41?style=for-the-badge&logo=microsoft)](https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-graphics)
[![Dear ImGui](https://img.shields.io/badge/UI-Dear%20ImGui%20v1.92-E65C00?style=for-the-badge&logo=gui)](https://github.com/ocornut/imgui)
[![Tailscale P2P](https://img.shields.io/badge/Network-Tailscale%20%2F%20Direct%20P2P-2D3748?style=for-the-badge&logo=tailscale)](https://tailscale.com)
[![Hardware Acceleration](https://img.shields.io/badge/Codec-NVENC%20%2F%20NVDEC%20%2F%20Opus-76B900?style=for-the-badge&logo=nvidia)](https://developer.nvidia.com/nvidia-video-codec-sdk)

</div>

## 📖 Overview

**Poupou P2P Stream** is a lightweight, high-performance desktop application designed for private, high-fidelity peer-to-peer screen streaming, crystal-clear voice communication, and interactive real-time screen collaboration.

Unlike traditional conferencing tools that rely on centralized relay servers and heavy web runtimes, **Poupou P2P Stream** connects peers directly using encrypted mesh networks (such as **Tailscale** or direct IP routing) and renders with bare-metal **DirectX 11** and **Dear ImGui** for sub-frame latency and minimal resource overhead.

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
| **Video Codec** | **NVIDIA NVENC / NVDEC** | Hardware-accelerated H.264 / AV1 video encoding and low-latency decoding |
| **UI Framework** | **Dear ImGui (v1.92.6)** | Immediate-mode GUI with custom styling, docking, and flexible viewport controls |
| **OS & Windowing** | **Win32 API / DWM** | Native Windows window management, borderless aero snaps, and DWM composition |
| **Audio Processing** | **RNNoise & WASAPI** | Neural noise suppression and low-latency audio capture/playback |
| **Audio Codec** | **Opus** | Ultra low-delay audio compression optimized for real-time speech |
| **Networking** | **P2P UDP / Tailscale** | Direct encrypted point-to-point data transport over a custom UDP protocol |
| **Vector Rendering** | **NanoSVG & DirectX 11** | High-DPI in-memory SVG parsing, rasterization, and GPU SRV texture generation |
| **Assets** | **Modern SVG Icons** | Scalable physical SVG vector icon set from icons.js.com |

---

## ⚡ Core Architecture & Engineering Specifications

### 🧵 1. Main Loop & Multi-Threaded Architecture

To guarantee absolute fluidity without blocking gameplay capture, audio processing, or user interface responsiveness, the application relies on **4 decoupled native worker threads**:

* **Main Thread (GUI / Overlay)**:
  * Manages the Win32 message loop, DirectX 11 swapchain presentation, and Dear ImGui rendering.
  * Dispatches keyboard/mouse inputs and handles user interactions.
* **Capture & Video Encoding Thread**:
  * Runs synchronized with the display refresh rate (e.g., 60 Hz or 144 Hz).
  * Executes desktop frame grabbing and dispatches textures to the hardware encoder.
* **Audio Thread**:
  * Captures real-time microphone audio samples in a continuous, uninterrupted loop.
  * Applies RNNoise filtering and encodes audio chunks on the fly.
* **Network UDP Worker Thread**:
  * Operates a non-blocking UDP socket to send and receive datagrams without ever stalling the capture or rendering loops.

---

### 📤 2. Transmitter Pipeline (Capture ➔ Compression ➔ Transmission)

The egress pipeline defines the dataflow from your local display and microphone to the remote peer:

```text
[ Screen ]  ──> DXGI (GPU) ──┐
                             ├──> Compositing (ImGui) ──> NVENC (GPU) ──> Packetizer ──┐
[ Drawing ] ──> Vectors ────┘                                                         ├──> UDP Socket (Tailscale)
                                                                                      │
[ Mic ]     ──> WASAPI ───────> Opus Encoder ─────────────────────────────────────────┘
```

1. **Desktop Frame Capture (DXGI)**:
   * Captures screen frames directly into GPU Video RAM (VRAM) as DirectX textures via the DXGI Desktop Duplication API.
2. **Overlay Compositing**:
   * If collaborative vector drawings or ImGui menus are active, they are rendered and merged directly onto this GPU texture surface.
3. **Hardware Video Encoding (NVENC)**:
   * The composite GPU texture is passed to the dedicated NVIDIA NVENC hardware engine to produce a high-efficiency, low-latency compressed bitstream (**H.264 / AV1**).
4. **Audio Capture & Encoding**:
   * Continuous raw PCM microphone audio is acquired through low-latency **WASAPI** and immediately compressed into lightweight packets using the **Opus** codec.
5. **Frame Slicing & Packetization**:
   * If an encoded video frame exceeds the network MTU (~1350 bytes), the packetizer fragments it into multiple slices.
   * Prepends a custom binary header to each slice: `[ Type_ID | Frame_Seq | Slice_Index | Total_Slices | Payload ]`.
6. **UDP Transmission**:
   * Slices are dispatched across the pre-configured UDP socket utilizing enlarged socket buffers to prevent packet drop under bursty traffic.

---

### 📥 3. Receiver Pipeline (Reception ➔ Decompression ➔ Display)

The ingress pipeline receives the incoming peer stream and unpacks it for real-time presentation:

```text
UDP Socket ──> Depacketizer ──┬──> De-Jitter Buffer ──> NVDEC (GPU) ──> Direct3D Texture ──> ImGui Viewport
                              ├──> Audio Buffer ─────> Opus Decoder ──> WASAPI Output    ──> Headphones
                              └──> Vector Manager ───> State Canvas (Vector Drawings)
```

1. **Depacketization & Demultiplexing**:
   * Receives incoming UDP datagrams and immediately demultiplexes them based on packet type (Video, Audio, Drawing/Command).
2. **Video Frame Reconstitution**:
   * Aggregates individual slices belonging to the same frame. As soon as all slices for a frame arrive, the complete bitstream buffer is dispatched to the hardware decoder (**NVDEC**).
3. **De-Jitter Buffering (Audio & Video)**:
   * A micro-buffer (a few milliseconds) re-orders packets if UDP delivered them out of sequence and eliminates packet arrival jitter.
4. **Hardware Decoding & Viewport Presentation**:
   * **Video**: Decoded via hardware (**NVDEC**) into a Direct3D GPU texture and displayed directly inside the ImGui stream panel.
   * **Audio**: Decoded via **Opus** and pushed to the default playback device (headphones) via low-latency **WASAPI**.
   * **Drawings & Vectors**: Received coordinate packets update the local vector canvas state for real-time collaborative strokes.

---

### 🌐 4. Network Protocol & Synchronization (UDP Logic)

To multiplex diverse data types across a single UDP socket without congestion or latency spikes, the protocol implements **3 virtual channels**:

* **Unreliable Stream Channel (Video Data)**:
  * If an individual slice or packet of a video frame is lost in transit, the receiver immediately discards that frame and waits for the next one to avoid head-of-line blocking and accumulated latency.
  * If consecutive packet loss exceeds a threshold, the receiver sends a high-priority `NEED_KEYFRAME` control signal requesting an immediate IDR/I-Frame from the transmitter.
* **Reliable Channel with ACK (Drawings & Interaction Commands)**:
  * Each drawing vector stroke or interface interaction event carries a monotonically incremented sequence number.
  * The receiver must reply with an `ACK` packet. If no acknowledgment is received within a millisecond timeout window, the transmitter re-sends the packet.
* **State Management & Heartbeat (Connection Liveness)**:
  * Automated `PING` / `PONG` packets are exchanged every second to measure round-trip latency and ensure connectivity.
  * If no packets are received for **3 to 5 seconds**, the client transitions into a **Disconnected** state and alerts the UI.

---

### 📦 5. Unified Packet Wire Structure

Every packet traveling over the UDP socket is packed in a compact binary format for zero-copy deserialization:

| Field | Size | Description & Role |
| :--- | :---: | :--- |
| **Type** | 1 byte | Channel identifier:<br>• `0x01` = Video Frame Slice<br>• `0x02` = Audio (Opus Packet)<br>• `0x03` = Drawing / Interaction Command<br>• `0x04` = Ping / Pong / Ack Control |
| **Sequence ID** | 4 bytes | Monotonically incremented sequence number to order packets and track loss |
| **Slice Info** | 2 bytes | Slicing descriptor: `[ Current Slice Index (1B) \| Total Slices (1B) ]` *(used for fragmented video frames)* |
| **Payload** | Variable | Raw binary payload (H.264/AV1 bitstream, Opus audio data, or vector coordinate data) |

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Type ID    |                 Sequence ID                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Seq ID (cont)|  Slice Index  |  Total Slices |               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+               +
|                                                               |
|                   Variable Payload Data ...                   |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

---

