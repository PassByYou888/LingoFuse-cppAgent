# LingoFuse-cppAgent

**Bring your existing C++ code into AI agent workflows — without rewriting it.**

Declare a function once. Get an AI-agent tool, a CLI command, and a cross-language plugin — automatically.

cppAgent is a C++ integration layer that borrows the LingoFuse mesh, the LingoFuse-Tools code generator, and a thin Python interface. It exists for one reason: C++ systems are fast, battle-tested, and hard to glue into modern AI workflows. cppAgent removes the glue.

---

## Who This Is For

You have working C++ code. It might be a pricing engine, a simulation core, a network scanner, a batch processor, or a legacy system that nobody wants to touch. You want AI agents, CLI users, and other languages to call it — without rewriting the code, without writing an MCP server, and without hand-maintaining an HTTP bridge.

This is what cppAgent is built for.

---

## What Actually Happens (Real Workflows)

### Quant Pricing Desk

A portfolio manager asks the internal AI assistant:

> "Run 10,000 Monte Carlo paths on Portfolio A and summarize the 95% VaR."

The assistant calls your C++ `run_monte_carlo` function through cppAgent. The result comes back in seconds. No one wrote an MCP server. No one maintained a REST wrapper. The function was declared once.

**Who does what:**
- **C++ maintainer**: Declares pricing functions.
- **Platform engineer**: Runs the agent runtime and local LLM service.
- **PM / Researcher**: Asks questions in natural language.

**What you get:** Fast, auditable risk analysis. Sensitive data stays inside the network.

---

### Cross-Language Tool Reuse

A data scientist works in Python. A front-end engineer works in JavaScript. Both need to call the same C++ simulation engine.

**Before cppAgent:** Two integration projects. Two sets of bindings. Two things to break when the C++ API changes.

**With cppAgent:** The C++ team declares the function once. The mesh handles the rest.

```python
from lingofuse import simulation
result = simulation.run_scenario(scenario_id="rate_shock_200bp", paths=50000)
```

```javascript
const result = await simulation.runScenario({
  scenario_id: "rate_shock_200bp",
  paths: 50000
});
```

Same function. Same result. No duplicated wrappers.

---

### Local / Private AI Agent

A security analyst needs to check a binary against internal malware signatures. The binary cannot leave the secure enclave.

The analyst asks the internal assistant. The assistant calls a local C++ scanning tool. The local LLM service processes only text and tool metadata. The binary never moves.

**Why this works:** `llm_service` runs a local model. `llm_proxy` forwards plain text to an approved internal endpoint. Nothing goes to a public cloud.

**Who needs this:** Financial services, healthcare, defense, any team with data residency rules.

---

### AI-Assisted C++ Development and CI

A developer opens a pull request that changes a lock-free queue. CI runs:

```bash
cppagent review --diff HEAD~1 --checks concurrency,memory,api
```

The CLI calls an approved LLM endpoint with the diff and symbol context. CI posts a review comment:

> "Potential ABA risk in `pop()`. `compare_and_swap` is called without a version tag. See `queue_stress_test`."

The developer can reproduce the finding locally. The code never leaves the internal network.

**What makes this possible:** You declared symbol lookup, call graph, test selection, and benchmark functions. cppAgent turned them into agent tools and CLI commands.

---

### Enterprise Agent Platform

A large enterprise has C++ systems everywhere: trading engines, real-time risk, simulation platforms, network appliances, legacy batch processors.

Instead of building a custom AI integration for each one, the platform team defines a standard cppAgent pattern. Each system declares its safe functions. LingoFuse provides cross-language RPC. The agent platform discovers tools, applies permissions, and logs calls.

**Result:** One integration pattern. Centralized audit. Reusable tools across agents. Lower maintenance cost than custom bridges.

---

## How It Works

### The Lifecycle

1. **Declare** a function in C++.
2. **cppAgent registers it** on the LingoFuse mesh.
3. **An AI agent calls it** like a built-in tool.
4. **Anyone else** — Python, JavaScript, CLI, CI — calls the same function through the mesh.

No MCP protocol boilerplate. No hand-written JSON Schema. No HTTP service.

### The Components

| Component | Language | Job |
|---|---|---|
| `agent_service` | C++ | Beacon. One per mesh. |
| `agent_api` | C++ | Tool provider. Replace with your functions. |
| `llm_cpp_tool` + `llm_client` | C++ | Client SDK + CLI / REPL. |
| `llm_proxy_tool.py` | Python | Server-side tool execution. Zero client changes. |

Supporting services: `llm_service.py`, `llm_proxy.py`, `bridge.py`, `mcp_api_tool.py`.

### One Call, End to End

```
Client asks: "What is 12 + 34?"
    ↓
LLM decides to call a tool
    ↓
Mesh routes to your C++ function
    ↓
add(12, 34) runs → 46
    ↓
LLM turns 46 into a reply
    ↓
Client receives: "46"
```

The client is unaware of the tool. Your C++ function is the only thing that actually runs.

---

## Build and Run

### Prerequisites

| Item | Version |
|---|---|
| **CMake** | 3.16+ |
| **C++ Compiler** | MSVC 2019+ (Windows), GCC 9+ (Linux), Clang 10+ (macOS) |
| **C++ Standard** | C++17 |
| **Python** | 3.10+ (for the Python service layer) |
| **LingoFuse** | Latest main branch |

### Step 1 — Clone LingoFuse

```bash
git clone https://github.com/PassByYou888/LingoFuse.git
```

The C++ interface directory is `LingoFuse/cpp/`. It contains the five header and source files required by cppAgent, plus runtime libraries in `LingoFuse/Binary/`.

### Step 2 — Configure cppAgent

Pass the path to the C++ interface directory via `-DLINGOFUSE_CPP_DIR`.

**Windows (PowerShell) — Visual Studio**

```powershell
cd LingoFuse-cppAgent\src
cmake -S . -B ..\x64 `
  -G "Visual Studio 17 2022" -A x64 `
  -DLINGOFUSE_CPP_DIR="..\..\LingoFuse\cpp"
```

**Linux / macOS — Makefiles**

```bash
cd LingoFuse-cppAgent/src
cmake -S . -B ../build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLINGOFUSE_CPP_DIR="../../LingoFuse/cpp"
```

`LINGOFUSE_CPP_DIR` must point to the directory containing `LingoFuse.h`, `LingoFuse.c`, `LingoFuse.hpp`, `lf_io.hpp`, and `json.hpp`. There is no auto-detection.

### Step 3 — Build

**Windows**

```powershell
cmake --build ..\x64 --config Release
```

**Linux / macOS**

```bash
cmake --build ../build -j
```

### Step 4 — Output

Executables are written into `src/`:

| Artifact | Type | Description |
|---|---|---|
| `agent_service` | Executable | Beacon / tool registry |
| `agent_api` | Executable | Tool provider template |
| `llm_cpp_tool` | Executable | C++ LLM CLI / REPL |
| `llm_client.lib` / `libllm_client.a` | Static library | Link into your C++ application |
| `lingofuse_c_wrapper.lib` / `.a` | Static library | C ABI dynamic loader |

### Step 5 — Place the Runtime Library

Copy the LingoFuse runtime library from `LingoFuse/Binary/` next to the executables:

| Platform | File |
|---|---|
| Windows | `LingoFuse64.dll` |
| Linux | `liblingofuse.so` |
| macOS | `liblingofuse.dylib` |

### Step 6 — Install Python Dependencies

```bash
pip install -r requirements.txt
```

### Step 7 — Run

```bash
# Four terminals
.\agent_service.exe          # beacon
.\agent_api.exe              # your tools
python llm_proxy_tool.py --backend-url http://127.0.0.1:1234/v1
.\llm_cpp_tool.exe --content "What is 12 + 34?" --keep
```

---

## Documentation

### Start Here

| Document | What it covers |
|---|---|
| [**QUICK_START.md**](QUICK_START.md) | Run the LLM in 15 minutes with the pre-built package. No compiler needed. |
| [**LingoFuse-cppAgent_Real_World_Application_Workflows.md**](LingoFuse-cppAgent_Real_World_Application_Workflows.md) | Real engineering workflows: quant pricing, cross-language reuse, private AI, AI-assisted C++ dev, enterprise platform. Who does the work, what triggers it, what the output looks like. |

### Architecture and Ecosystem

| Document | What it covers |
|---|---|
| [LingoFuse_LLM_Ecosystem_User_Guide.md](src/LingoFuse_LLM_Ecosystem_User_Guide.md) | Four core application components, two tool execution paths, capability matrix, streaming protocol |
| [LingoFuse_LLM_Proxy_Compatibility_Guide.md](src/LingoFuse_LLM_Proxy_Compatibility_Guide.md) | 250+ OpenAI-compatible backends (cloud APIs, local servers, gateways, desktop clients) |

### Component Reference

| Document | What it covers |
|---|---|
| [LingoFuse_LLM_Service_CLI_guide.md](src/LingoFuse_LLM_Service_CLI_guide.md) | Local inference service (`llm_service`) — text-only, embeddable |
| [LingoFuse_LLM_Proxy_CLI_Guide.md](src/LingoFuse_LLM_Proxy_CLI_Guide.md) | Pure text forwarder (`llm_proxy`) — no tools |
| [LingoFuse_LLM_Proxy_Tool_CLI_Guide.md](src/LingoFuse_LLM_Proxy_Tool_CLI_Guide.md) | Tool execution bridge (`llm_proxy_tool` / LTB) — server-side tools |
| [llm_cpp_tool_User_Guide.md](src/llm_cpp_tool_User_Guide.md) | C++ command-line client — REPL, attachments, Structured Output |

### Model and Bridge

| Document | What it covers |
|---|---|
| [NVIDIA-Nemotron-3-Nano-Omni-30B-A3B-Reasoning-UD-IQ4_XS.md](src/NVIDIA-Nemotron-3-Nano-Omni-30B-A3B-Reasoning-UD-IQ4_XS.md) | Recommended multimodal model — download, quantization, deployment |
| [Bridge_User_Guide.md](src/lingofuse/Bridge_User_Guide.md) | HTTP ↔ LingoFuse gateway and JSON repair service |

---

## Ecosystem

| Repository | Role |
|---|---|
| [LingoFuse](https://github.com/PassByYou888/LingoFuse) | Core cross-language mesh |
| [LingoFuse-Tools](https://github.com/PassByYou888/LingoFuse-Tools) | Declaration → code generator |
| [LingoFuse-pasAgent-v3](https://github.com/PassByYou888/LingoFuse-pasAgent-v3) | Reference agent implementation |
| [LingoFuse-pasAgent](https://github.com/PassByYou888/LingoFuse-pasAgent) | Earlier generation |

---

## License

MIT.

---

**Maintainer**: LingoFuse Team
**Status**: Active development
