# llm_cpp_tool — User Guide

> **Applies to**: `llm_cpp_tool` (Windows / Linux / macOS)
> **Runtime dependency**: LingoFuse library (`LingoFuse64.dll` / `liblingofuse.so` / `liblingofuse.dylib`)
> **Server compatibility**: any of `llm_service` / `llm_proxy` / `llm_proxy_tool`

---

## 1. Overview

`llm_cpp_tool` is a command-line client for LingoFuse LLM services. It provides two modes and one optional advanced feature.

**Two modes:**

- **One-shot** — pass `--content`, `--text`, or `--image`; the program issues a single request, streams the full response, and exits.
- **Interactive (REPL)** — launch without those arguments; converse across multiple turns, switch sessions, attach files.

**One advanced feature:**

- **Structured Output** — load a JSON Schema and have the backend constrain its reply to that schema. Works against `llm_proxy` and `llm_proxy_tool`. Does **not** work against `llm_service`.

Both modes share:

- Streaming `chunk` output (printed as it arrives)
- Streaming `think` output (dimmed when the terminal supports ANSI)
- Multiple sessions
- Text and image attachments (in one-shot mode and via interactive commands)

**Event model.** The program runs a single main thread that owns the console. It polls a queue for user input and drains LLM events via `pumpEvents()`. Handlers fire on the main thread, so streaming output and user input never interleave.

---

## 2. Preparation

### 2.1 Runtime library

Place the LingoFuse runtime library next to the `llm_cpp_tool` executable:

| Platform | File |
|---|---|
| Windows | `LingoFuse64.dll` |
| Linux | `liblingofuse.so` |
| macOS | `liblingofuse.dylib` |

If it is missing:

```
[Client] LF_LoadLibrary failed: ...
[Client] Place LingoFuse64.dll / liblingofuse.so next to this executable...
```

### 2.2 Server

At least one LLM server must be running: `llm_service` (local inference), `llm_proxy` (pure forwarder), or `llm_proxy_tool` (forwarder + server-side tools).

Capability-dependent features:

| Feature | Requires |
|---|---|
| Image attachments | Server started with `--vision` (proxy kinds) and a vision-capable backend. Not supported by `llm_service`. |
| Structured Output | `llm_proxy` or `llm_proxy_tool` (not `llm_service`). |
| `set_system_message` | `llm_service` only. |

---

## 3. Command-line arguments

### 3.1 Connection

| Argument | Default | Description |
|---|---|---|
| `--endpoint <addr>` | `ipc:llm_service` | LingoFuse endpoint |
| `--server-app <name>` | `LLM_Service` | Target App name |
| `--timeout <ms>` | `30000` | Per-call timeout in milliseconds |

### 3.2 One-shot mode

Supplying any of `--content`, `--text`, or `--image` enters one-shot mode.

| Argument | Description |
|---|---|
| `--content <text>` | Content to send |
| `--prompt <text>` | Extra prompt appended after `--content` |
| `--text <file>` | Attach a text file (repeatable) |
| `--image <file>` | Attach an image file (repeatable) |
| `--session-id <id>` | Reuse an existing session instead of creating one |
| `--system-message <text>` | System message for a new session |
| `--keep` | Keep the session alive after the turn (default: close it) |
| `--thinking` | Enable thinking for this turn (effect depends on the server) |

### 3.3 Structured Output

Available in both modes. A schema loaded at startup persists into the interactive REPL.

| Argument | Description |
|---|---|
| `--schema <file>` | Load JSON Schema from a file |
| `--schema-name <name>` | Schema name (default: `response_schema`) |
| `--no-strict` | Disable strict mode (default: strict is ON) |

A bare `--schema` **without** `--content` / `--text` / `--image` still enters interactive mode — the schema is loaded and stays active for typed prompts.

### 3.4 Diagnostics

| Argument | Description |
|---|---|
| `--debug` | Print each input line as hex bytes to stderr |
| `--help`, `-h` | Print usage and exit |

---

## 4. One-shot mode

### 4.1 Basic usage

```bash
llm_cpp_tool --content "explain quicksort" --prompt "in one paragraph"
```

Sequence:

1. Connect to the server.
2. Create a session (unless `--session-id` is given).
3. Send the `generate` request.
4. Stream `chunk` / `think` events.
5. On `finish`, close the session (unless `--keep`) and exit.

### 4.2 Text attachments

```bash
llm_cpp_tool --content "review this code" --text main.cpp --text utils.cpp
```

Multiple `--text` flags are allowed and preserve command-line order. The files are merged into the user message as `<attachment name="...">` blocks.

Size limits (enforced client-side before sending):

| Scope | Limit |
|---|---|
| Per text file | 256 KB |
| Per request, all text files combined | 512 KB |

### 4.3 Image attachments

```bash
llm_cpp_tool --content "describe this chart" --image chart.png
```

Multiple `--image` flags are allowed. Images are base64-encoded and sent as OpenAI `image_url` content parts.

MIME is inferred from the extension:

| Extension | MIME |
|---|---|
| `.png` | `image/png` |
| `.jpg` / `.jpeg` | `image/jpeg` |
| `.webp` | `image/webp` |
| others | `image/png` |

Size limits:

| Scope | Limit |
|---|---|
| Per image (base64) | 8 MB |
| Per request, all images combined (base64) | 16 MB |

### 4.4 Mixing text and image

```bash
llm_cpp_tool --content "review" --text spec.md --image design.png
```

### 4.5 Reusing a session

```bash
llm_cpp_tool --session-id <id> --content "continue"
```

Combined with `--keep`:

```bash
llm_cpp_tool --content "first question" --keep
# output prints: [Client] Session <id> kept alive.
llm_cpp_tool --session-id <id> --content "second question"
```

### 4.6 Structured Output in one-shot mode

```bash
llm_cpp_tool \
  --content "detect all objects in this image" \
  --image photo.png \
  --schema detector.json \
  --schema-name object_detection
```

The schema JSON is loaded, validated (must be a JSON object), and sent as `options.response_format`.

---

## 5. Interactive mode

Launch without `--content` / `--text` / `--image` to enter the REPL.

```
llm_cpp_tool
```

On entry the program connects, creates a fresh session, and prints `> `. Plain text starts a turn; a line starting with `/` is a command.

### 5.1 Session commands

| Command | Description |
|---|---|
| `/new` | Create a new session and switch to it. Clears pending attachments. The active schema is **kept**. |
| `/use <session_id>` | Switch to the given session |
| `/sessions` | List this client's sessions (current one marked `*`) |
| `/close [id]` | Close the given session (default: current). Closing the current session clears pending attachments. |
| `/cancel` | Cancel the generation in progress |

### 5.2 Attachment commands

| Command | Description |
|---|---|
| `/text <file>` | Attach a text file to the next turn |
| `/image <file>` | Attach an image file to the next turn |
| `/attach` | Show pending attachments and the active schema status |
| `/clear` | Clear all pending attachments |

Attachments are **one-shot**: the file is read and encoded immediately; it is delivered with the next plain-text turn and then the pending list is cleared. `/new` and closing the current session also clear the list.

### 5.3 Structured Output commands

The schema is **sticky**: once loaded, it applies to every subsequent turn until disabled.

| Command | Description |
|---|---|
| `/schema <file>` | Load JSON Schema from a file |
| `/schema template` | Load the built-in object-detection template |
| `/schema off` | Disable Structured Output |
| `/schema` or `/schema show` | Show the current schema status |
| `/schema-name <name>` | Set the schema name (applies on the next turn) |
| `/strict on\|off` | Toggle strict mode |

The built-in template (`/schema template`) targets object detection: an array of `{label, bbox[4], confidence}` objects with normalized coordinates.

### 5.4 Server settings

| Command | Description |
|---|---|
| `/sys <message>` | Update the global default system message (`llm_service` only) |
| `/health` | Query server health |
| `/capabilities` | Show the capability matrix |
| `/capabilities refresh` | Force a fresh capability fetch |
| `/thinking on\|off` | Client-side toggle. In the current revision the server controls thinking; the command prints an explanatory message. |

### 5.5 Help and exit

| Command | Description |
|---|---|
| `/help` | Show all commands |
| `/quit`, `/exit` | Exit |

### 5.6 Complete example

```
> /schema template
[Client] Loaded detector template: name=object_detection, strict=on, body=612 chars.

> /image chart.png
[Client] Attached image: chart.png (184320 base64 chars, mime=image/png)

> /attach
[Client] Pending attachments:
    [image] chart.png (184320 base64 chars)
[Client] Structured Output: enabled. name=object_detection, strict=on, body=612 chars.

> detect all objects and return JSON
[Client] Session 74ed43aa-... queued, streaming...
------------------------------------------------------------
<streamed model output>
------------------------------------------------------------
[Client] Attachments cleared after send (texts=0, images=1).
[Client] Turn finished (reason=stop)

> /schema off
[Client] Structured Output disabled.

> /quit
[Client] Shutting down...
[Client] Done.
```

### 5.7 Behavior during an active turn

While a turn is in progress:

- The `> ` prompt is not printed.
- Any input — command or plain text — is rejected with `[Client] A turn is running. Please wait for it to finish.` and discarded.
- Streaming output continues normally.
- When the `finish` event arrives, the prompt returns.

---

## 6. Structured Output details

### 6.1 Dispatch logic

Four combinations are handled transparently by a single internal dispatcher:

| Schema | Attachments | Path |
|:---:|:---:|---|
| no | no | Plain generate |
| no | yes | Generate with attachments |
| yes | no | Generate with JSON Schema |
| yes | yes | Generate with attachments + JSON Schema |

The client is not aware of which path is taken; all four produce identical streaming behavior.

### 6.2 Schema validation

The schema body is validated **before** the request is sent:

- Must parse as JSON.
- Must be a JSON object (not an array, string, number, or null).

Invalid schemas are rejected client-side with a precise error. `--schema template` and `/schema template` bypass file loading and use the built-in detector template.

### 6.3 Server-side support

Structured Output works against `llm_proxy` and `llm_proxy_tool`. It does **not** work against `llm_service` — the local inference path does not implement the `response_format` field.

### 6.4 Interaction with server-side tools

When `llm_proxy_tool` is running with tools enabled, some tool-capable models prefer `tool_calls` over the schema when both are present. If a schema conflict is observed:

- Run the server with `--no-tools` to make it a pure passthrough.
- Or use a schema that does not conflict with the tool-call intent.

---

## 7. Encoding

### 7.1 Input encoding

| Platform | Method |
|---|---|
| Windows | `ReadConsoleW` + UTF-8 conversion (bypasses the CRT's ANSI code page) |
| Linux / macOS | `std::getline` (already UTF-8 aware) |

Use `--debug` to inspect input bytes. `你好` appears as:

```
[DEBUG] line bytes: E4 BD A0 E5 A5 BD
```

### 7.2 Output encoding

- On Windows, the console is switched to UTF-8 (code page 65001) at startup.
- ANSI virtual terminal processing is enabled where available, so the dim `think` style renders correctly.
- Styling is disabled when the terminal does not support ANSI or when `NO_COLOR` is set.

### 7.3 Environment variables

| Variable | Effect |
|---|---|
| `NO_COLOR` | Set to any non-empty value to disable ANSI styling (dim `think` output) |

---

## 8. Typical scenarios

### 8.1 Quick question

```bash
llm_cpp_tool --content "What is LingoFuse?"
```

### 8.2 Code review

```bash
llm_cpp_tool \
  --content "Review this code. Point out bugs and performance issues." \
  --text src/main.cpp \
  --text src/utils.cpp
```

### 8.3 Image detection (structured output)

```bash
llm_cpp_tool \
  --content "Detect all objects. Return normalized coordinates." \
  --image photo.jpg \
  --schema detector.json \
  --schema-name object_detection
```

Requires `llm_proxy` / `llm_proxy_tool` started with `--vision` and a vision-capable backend.

### 8.4 Multi-turn via `--keep`

```bash
llm_cpp_tool --content "Let's discuss a problem." --keep
# output prints: [Client] Session <id> kept alive.
llm_cpp_tool --session-id <id> --content "Continuing from above"
```

### 8.5 Interactive exploration

```bash
llm_cpp_tool

> Write a Python binary search function
<model output>

> /image data.png
[Client] Attached image: data.png (...)

> /schema detector.json
[Client] Schema loaded: name=response_schema, strict=on, body=...

> Analyze this image and return JSON
<model output>

> /quit
```

---

## 9. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `LF_LoadLibrary failed` | Runtime library not on the search path | Place it next to the executable |
| `Connect failed: LF_PrepareClient returned -1` | Endpoint already in use, or server not running | Use `Overlap_Connection=True` on the server, or a different endpoint |
| `Connect failed: LF_PrepareDone failed` | Framework initialization failed | Verify the runtime library version matches the header |
| `empty response from API generate (timeout or target unreachable)` | Server not running, or `--server-app` wrong | Start the correct server, or fix `--server-app`; increase `--timeout` |
| Image attachment rejected (`code: -1`) | Backend is `llm_service` (no vision), or the proxy was started without `--vision`, or the size/MIME limit is exceeded | Use `llm_proxy` / `llm_proxy_tool` with `--vision`; use `.png` / `.jpg` / `.jpeg` / `.webp`; stay under 8 MB per image and 16 MB total |
| Schema rejected | Not a JSON object, or file missing | Validate the schema file; check the schema body is a JSON object |
| Structured Output has no effect | Backend is `llm_service` | Use `llm_proxy` or `llm_proxy_tool` |
| Chinese input shows garbled text | Input went through a non-UTF-8 pipe, or the terminal locale is not UTF-8 | Check with `--debug`; on Linux/macOS verify `LANG` includes UTF-8 |
| Output has no color | `NO_COLOR` is set, or the terminal does not support ANSI | Unset `NO_COLOR`; use a VT-capable terminal |
| Program stuck with no prompt | No `finish` event arrived | Check the server console log; Ctrl+C may not shut down cleanly in the current revision — closing the terminal is a reliable fallback |

---

## 10. Relationship to other tools

| Tool | Description |
|---|---|
| `llm_cpp_tool` | This tool. C++ command-line client. |
| `llm_test.py` | Python interactive client. Similar feature set. |
| Client SDK (any language) | Event-driven LLM client library. Functionally equivalent to the C++ `llm_client` library used here. |
| `agent_service` | LingoFuse agent beacon. Not used by this tool. |
| `agent_api` | Sample arithmetic tool provider. Not used by this tool. |

---

## 11. Related topics

| Topic | Notes |
|---|---|
| `llm_client.hpp` / `llm_client.cpp` | Client library implementation; contains the semantics of every Call API |
| `llm_service` / `llm_proxy` / `llm_proxy_tool` CLI guides | The three server kinds |
| `CMakeLists.txt` | Build script; shows how to point `-DLINGOFUSE_CPP_DIR=<path>` at the LingoFuse C++ interface directory |

---

*End of document*