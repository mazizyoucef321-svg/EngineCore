#  EngineCore

**EngineCore** is a lightweight, low-level performance and frame-pacing optimization engine tailored specifically for low-end hardware and integrated GPUs (such as **Intel UHD Graphics**).

By interfacing directly with Event Tracing for Windows (ETW) and dynamically tuning thread affinity, EngineCore eliminates micro-stutters, stabilizes frame delivery times, and drastically cuts down DPC latency / input lag without overhead.

---

##  Target Hardware & Real-World Results

Designed for resource-constrained systems where every millisecond counts:
* **Tested Environment:** Intel UHD Integrated Graphics
* **Primary Objective:** Frame-pacing stabilization and micro-stutter elimination
* **Validated Titles:**
  * **Team Fortress 2:** Locked 16.46ms frame time (60 FPS floor) with zero stuttering.
  * **Asphalt Legends:** Stable frame delivery under heavy VFX stress (Unlimited FPS mode yielding zero micro-stutters).

---

##  Core Architecture & Features

1. **`PresentMon_hooks.hpp` (ETW Telemetry):** Intercepts low-level DXGI swap-chain events via ETW for real-time telemetry without polling overhead.
2. **`CoreVault` (Thread & DPC Steering):** Isolates critical rendering threads and manages CPU core affinity to prevent context-switching delays.
3. **`SafetyValve`:** Dynamically manages frame queues to stabilize frame delivery and prevent input queue buffering.

---

##  Getting Started

### Prerequisites
* Windows 10 / 11 (x64)
* Visual Studio / VS Code with CMake support
* Administrator Privileges (Required for ETW kernel event tracing)

### Building from Source (Release Mode)
For maximum throughput and optimal low-level optimization:

```powershell
# Generate CMake build files in Release mode
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build build --config Release