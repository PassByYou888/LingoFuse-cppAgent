# llm_cpp_tool User Guide

> Version: matches the current implementation of `llm_cpp_tool.cpp`
> Supported platforms: Windows / Linux / macOS
> Runtime dependency: LingoFuse library (`LingoFuse64.dll` / `liblingofuse.so` / `liblingofuse.dylib`)
> Server: any of `llm_service.py` / `llm_proxy.py` / `llm_proxy_tool.py`

---

## 1. Overview

`llm_cpp_tool` is a command-line tool for talking to a LingoFuse LLM service. It is the C++ counterpart of `llm_test.py`, with one additional capability: **uploading image files** as part of a conversation.

It has two operating modes:

- **One-shot mode**: pass `--content`, `--text`, or `--image` on the command line. The program issues a single request, streams the full response, and exits.
- **Interactive mode (REPL)**: launch without those arguments to enter a prompt. You can hold multi-turn conversations, switch sessions, and attach text or image files.

Both modes support:
- Streaming output (`chunk` events printed as they arrive)
- Reasoning stream (`think` events rendered in a dim style, when the server emits them)
- Multiple sessions
- Text and image attachments

---

## 2. Preparation

### 2.1 Runtime library

Copy the LingoFuse runtime library next to the `llm_cpp_tool` executable:

| Platform | File name              |
|----------|------------------------|
| Windows  | `LingoFuse64.dll`      |
| Linux    | `liblingofuse.so`      |
| macOS    | `liblingofuse.dylib`   |

If the runtime library is missing, the program prints:

```
[Client] LF_LoadLibrary failed: ...
[Client] Place LingoFuse64.dll / liblingofuse.so next to this executable...
```

### 2.2 Server

At least one LLM server must be running:

- `llm_service.exe` (local inference; default endpoint `ipc:llm_service`, App name `LLM_Service`)
- `llm_proxy.exe` (forwarder to an OpenAI-compatible backend)
- `llm_proxy_tool.exe` (proxy with server-side tool execution)

Image attachments are accepted only when the server advertises support for the `attachments` field. Otherwise the server rejects the request with `code: -1` and a specific error message.

---

## 3. Command-line arguments

### 3.1 Connection

| Argument | Default | Description |
|----------|---------|-------------|
| `--endpoint <addr>` | `ipc:llm_service` | LingoFuse endpoint |
| `--server-app <name>` | `LLM_Service` | Target App name |
| `--timeout <ms>` | `30000` | Per-call timeout in milliseconds |

### 3.2 One-shot mode triggers

Supplying any of the following switches the program into one-shot mode:

| Argument | Description |
|----------|-------------|
| `--content <text>` | Content to send |
| `--text <file>` | Attach a text file (repeatable) |
| `--image <file>` | Attach an image file (repeatable) |

### 3.3 One-shot auxiliary arguments

| Argument | Description |
|----------|-------------|
| `--prompt <text>` | Extra prompt appended after `--content` |
| `--session-id <id>` | Reuse an existing session instead of creating a new one |
| `--system-message <text>` | System message for a new session |
| `--keep` | Keep the session alive after the request (default: close it) |
| `--thinking` | Enable thinking mode (effect depends on the server) |

### 3.4 Diagnostics

| Argument | Description |
|----------|-------------|
| `--debug` | Print each input line as hex bytes to stderr (for encoding diagnostics) |
| `--help`, `-h` | Print usage and exit |

---

## 4. One-shot mode

### 4.1 Basic usage

```bash
llm_cpp_tool --content "print('hello')" --prompt "Explain this code"
```

Sequence:

1. Connect to the server.
2. Create a new session (unless `--session-id` is given).
3. Send the `generate` request.
4. Stream `chunk` / `think` events to the terminal.
5. On the `finish` event, close the session and exit.

### 4.2 Attaching text files

```bash
llm_cpp_tool --content "review this code" --text main.cpp
```

`--text` may be repeated. Multiple files are attached in command-line order.

Text size limits:
- Per file: ≤ 256 KB
- Per request: ≤ 512 KB total

When a limit is exceeded, the program rejects the request **before** sending and prints a precise error.

### 4.3 Attaching image files

```bash
llm_cpp_tool --content "describe this chart" --image chart.png
```

`--image` may be repeated.

Supported image MIME types (inferred from the extension):
- `.png` → `image/png`
- `.jpg` / `.jpeg` → `image/jpeg`
- `.webp` → `image/webp`

Other extensions default to `image/png`. The server will reject the request if the MIME type is not in its whitelist.

Image size limits:
- Per image: base64 ≤ 8 MB (roughly 6 MB of raw data)
- Per request: base64 ≤ 16 MB total

### 4.4 Attaching both text and image

```bash
llm_cpp_tool --content "review" --text spec.md --image design.png
```

Text attachments are merged into the user message as `<attachment>` blocks. Images are sent as multi-modal `image_url` parts.

### 4.5 Reusing an existing session

```bash
llm_cpp_tool --session-id 74ed43aa-e68d-42eb-ab16-9778033da1e6 --content "continue"
```

Combined with `--keep`:

```bash
llm_cpp_tool --content "first question" --keep
llm_cpp_tool --session-id <session_id_from_output> --content "second question"
```

### 4.6 Sample output

```
[Client] Connecting to ipc:llm_service (server_app=LLM_Service) ...
[Client] Connected (client_name=@__generate__@ipc:llm_service:0&...)
[Client] Created session 74ed43aa-e68d-42eb-ab16-9778033da1e6
[Client] Session 74ed43aa-e68d-42eb-ab16-9778033da1e6 queued, streaming...
------------------------------------------------------------
<streamed model output>
------------------------------------------------------------
[Client] Turn finished (reason=stop)
[Client] Session closed.
[Client] Done.
```

---

## 5. Interactive mode

Launch without `--content` / `--text` / `--image` to enter the REPL:

```bash
llm_cpp_tool
```

On entry the program:

1. Connects to the server.
2. Creates a fresh session.
3. Prints the prompt `> `.

Typing plain text at the prompt starts a turn. A line beginning with `/` is treated as a command.

### 5.1 Session management commands

| Command | Description |
|---------|-------------|
| `/new` | Create a new session and switch to it. **Clears any pending attachments.** |
| `/use <session_id>` | Switch to the given session |
| `/sessions` | List this client's sessions (the current one is marked with `*`) |
| `/close [id]` | Close the given session (default: current). **Closing the current session clears pending attachments.** |
| `/cancel` | Cancel the generation currently in progress |

### 5.2 Attachment commands

| Command | Description |
|---------|-------------|
| `/text <file>` | Attach a text file to the **next** turn |
| `/image <file>` | Attach an image file to the **next** turn |
| `/attach` | List the currently pending attachments |
| `/clear` | Clear all pending attachments |

**Attachment lifecycle:**

1. `/text` and `/image` add the file to the pending list. The file is read and encoded in memory immediately.
2. The next **ordinary input line** is sent together with all pending attachments.
3. After a successful send, the pending list is **cleared automatically**.
4. `/new` and `/close` (when closing the current session) also clear the pending list, so attachments are never accidentally delivered to a different session.

### 5.3 Server settings commands

| Command | Description |
|---------|-------------|
| `/sys <message>` | Update the global default system message (**only supported by `llm_service`**) |
| `/health` | Query the server health |
| `/capabilities` | Show the server capability matrix |
| `/capabilities refresh` | Force a fresh capability fetch |
| `/thinking on\|off` | Toggle thinking mode (in the current revision the server controls this; the client only reports) |

### 5.4 Help and exit

| Command | Description |
|---------|-------------|
| `/help` | Show all available commands |
| `/quit`, `/exit` | Exit the program |

### 5.5 Complete interactive example

```
> /help
Commands:
  /new                      Create a new session
  /use <session_id>         Switch the current session
  /sessions                 List sessions for this client
  /close [id]               Close a session (default: current)
  /cancel                   Cancel the current generation
  /sys <message>            Update the global system message
  /health                   Query the server health
  /capabilities             Show the server capability matrix
  /capabilities refresh     Force a fresh capability fetch
  /thinking on|off          Toggle thinking mode
  /text <file>              Attach a text file to the next turn
  /image <file>             Attach an image file to the next turn
  /attach                   List currently attached files
  /clear                    Clear all attached files
  /help                     Show this help
  /quit, /exit              Quit
Anything else is sent to the current session as user input.
If any attachments are pending, they are sent with that turn
and then cleared automatically.

> /image chart.png
[Client] Attached image: chart.png (184320 base64 chars, mime=image/png)

> /text labels.txt
[Client] Attached text: labels.txt (42 chars, mime=text/plain)

> /attach
[Client] Pending attachments:
    [text]  labels.txt (42 chars)
    [image] chart.png (184320 base64 chars)

> Detect all objects in the image and return JSON
[Client] Session 74ed43aa-... queued, streaming...
------------------------------------------------------------
<streamed model output>
------------------------------------------------------------
[Client] Attachments cleared after send (texts=1, images=1).
[Client] Turn finished (reason=stop)

> /quit
[Client] Shutting down...
[Client] Done.
```

### 5.6 Behaviour during an active turn

While the model is generating (`turn_active == true`):

- The `> ` prompt is **not** printed.
- Any input at this time — whether a command or plain text — is rejected with:

  ```
  [Client] A turn is running. Please wait for it to finish.
  ```

  The input is discarded; wait for the current turn to finish.
- Streaming output continues normally.

---

## 6. Encoding

### 6.1 Input encoding

- **Windows**: uses `ReadConsoleW` to read wide characters, then converts to UTF-8. This bypasses the CRT's ANSI code page conversion, which would otherwise mangle non-ASCII input.
- **Linux / macOS**: uses `std::getline`, which is already UTF-8 aware.

If you suspect input corruption, launch with `--debug`. The program will print each input line as hex bytes to stderr. For example, `你好` should appear as:

```
[DEBUG] line bytes: E4 BD A0 E5 A5 BD
```

### 6.2 Output encoding

- On Windows, the console is switched to UTF-8 (code page 65001) at startup.
- ANSI virtual terminal processing is enabled where available, so the dim style used for `think` output (`\033[2m`) renders correctly.
- If the terminal does not support ANSI, or if `NO_COLOR=1` is set, styling is disabled.

---

## 7. Environment variables

| Variable | Effect |
|----------|--------|
| `NO_COLOR` | Set to any non-empty value to disable ANSI styling (the `think` stream is no longer dimmed) |

---

## 8. Typical usage scenarios

### 8.1 Quick question

```bash
llm_cpp_tool --content "What is LingoFuse?"
```

### 8.2 Code review (with text attachments)

```bash
llm_cpp_tool \
  --content "Review this code. Point out bugs and performance issues." \
  --text src/main.cpp \
  --text src/utils.cpp
```

### 8.3 Image detection (with an image attachment)

```bash
llm_cpp_tool \
  --content "Detect all objects in this image and return normalized coordinates." \
  --image photo.jpg
```

> Requires the server (`llm_proxy` / `llm_proxy_tool`) to be started with `--vision`, and the backend to be a vision-capable model.

### 8.4 Multi-turn conversation (keeping the session id)

```bash
# First turn: send and keep the session alive
llm_cpp_tool --content "Let's discuss a problem." --keep

# The output prints the session id, e.g.:
# [Client] Session 74ed43aa-e68d-42eb-ab16-9778033da1e6 kept alive.

# Second turn: continue the session
llm_cpp_tool --session-id 74ed43aa-e68d-42eb-ab16-9778033da1e6 --content "Continuing from above"
```

### 8.5 Interactive exploration

```bash
llm_cpp_tool
> Write a Python binary search function
<model output>
> /image data.png
[Client] Attached image: data.png (...)
> Analyze this image
<model output>
> /quit
```

---

## 9. Troubleshooting

### 9.1 `LF_LoadLibrary failed`

- Ensure `LingoFuse64.dll` (Windows) or `liblingofuse.so` (Linux) is in the executable's directory or on the system library search path.

### 9.2 `Connect failed: LF_PrepareClient returned -1`

- The endpoint address is already in use. By default `ipc:llm_service` accepts a single client. To allow multiple clients, start the server with `Overlap_Connection=True`, or use a different endpoint.
- Or the target server is not running.

### 9.3 `Connect failed: LF_PrepareDone failed`

- LingoFuse framework initialization failed. Verify that the runtime library version matches `LingoFuse.h`.

### 9.4 `empty response from API generate (timeout or target unreachable)`

- The server is not running, or `--server-app` is wrong.
- Increase `--timeout`.

### 9.5 Image attachment rejected

The server returned `code: -1`. Possible causes:

1. The target is `llm_service`; the local inference path does not support multi-modal input.
   → Use `llm_proxy` or `llm_proxy_tool`.
2. The target proxy server was started without `--vision`.
   → Add `--vision` to the server's startup arguments.
3. The image exceeds a size limit.
   → Check the base64 length printed by the program: ≤ 8 MB per image, ≤ 16 MB total.
4. The MIME type is not on the whitelist.
   → Use only `.png` / `.jpg` / `.jpeg` / `.webp`.

### 9.6 Chinese input shows garbled text

- Confirm the input is not coming through a pipe or another intermediary tool.
- Launch with `--debug` and inspect the hex bytes of the input line. `你好` should be `E4 BD A0 E5 A5 BD`.
- On non-Windows systems, confirm the terminal's locale is UTF-8 (`echo $LANG`).

### 9.7 Output has no colour

- If `NO_COLOR=1` is set, styling is disabled. Check whether it is set unintentionally.
- Older Windows terminals (such as a `cmd.exe` without VT enabled) may not support ANSI.

### 9.8 Program stuck and does not return the prompt

- If the model produced output but no `finish` event arrives, the server may have failed. Inspect the server console log.
- Press Ctrl+C to interrupt (note: in the current revision Ctrl+C is not guaranteed to shut down cleanly; you may need to close the terminal).

---

## 10. Relationship to other tools

| Tool | Description |
|------|-------------|
| `llm_cpp_tool` | This tool. C++ command-line client. |
| `llm_test.py` | Python interactive client. Similar feature set, but limited image attachment support. |
| `llm_client_v3.pas` | Pascal client library; functionally equivalent to this tool's `llm_client` C++ library. |
| `agent_service` | LingoFuse agent beacon. Not used by this tool. |
| `agent_api` | Sample arithmetic tool provider. Not used by this tool. |

---

## 11. Related documentation

- `llm_client.hpp` / `llm_client.cpp`: client library implementation, containing the semantics of every Call API.
- `llm_service.py` / `llm_proxy.py` / `llm_proxy_tool.py`: the three server kinds.
- `CMakeLists.txt`: build script; shows how to point `-DLINGOFUSE_CPP_DIR=<path>` at the LingoFuse C++ interface directory.

---

*End of document*
