# hal-genai-example-v2

Local HTTP demo for HAL GenAI (`HAL_GENAI_OPS`): **LLM** or **VLM** HEF, browser chat, **SSE** token streaming, **Stop** (abort generation), and **Clear context** (device KV via HAL + server message list).

## Build

Requires a Hailo SDK sysroot with **HailoRT GenAI** headers (`hailo/genai/llm/llm.hpp`) and `libhailort`, same as other `hal_v2` Hailo-15 builds (cross-compile with the Poky SDK toolchain recommended).

```bash
cd hal_v2/build-hailo15   # your CMake build dir
cmake --build . --target hal-genai-example-v2
```

The target is created only when `HAVE_HAL_GENAI` is on (GenAI headers found next to HailoRT). Rebuild **`libaipc_hal` / `hal-hailo15-model`** after HAL GenAI changes, then rebuild this example.

## Run (on device)

```bash
# VLM: optional leading "serve"; --kind or -kind
./hal-genai-example-v2 serve --hef ./Qwen2-VL-2B-Instruct.hef --kind vlm --port 9000

# LLM
./hal-genai-example-v2 serve --hef /path/to/model.hef --kind llm --host 0.0.0.0 --port 8080
# optional: --vdevice-group <group_id>
```

### Manifest (zoo-style `manifest.json`)

Pass **`--manifest <manifest.json>`** or a **directory** that contains `manifest.json` (same layout as `hailo_model_zoo_genai/models/manifests/...`).

Supported fields include:

- **`type`**: `llm` or `vlm`
- **`hef_path` / `model_path` / `hef`**: model path
- **`host`**, **`port`**, **`vdevice_group`**
- **`vlm_params`**: `frame_width`, `frame_height` (decode target; if both are set in the manifest, manifest wins over CLI `--vlm-frame-w/h`. If the pixel count does not match the HEF, the server may fall back to the SDK layout or decode at manifest size then linear-resize to the SDK frame, depending on configuration; see stderr messages.)
- **`generation_params`**: `temperature`, `top_p`, `top_k`, `max_tokens` / `max_generated_tokens`, `frequency_penalty`, `do_sample`, `stop_tokens`
- **`template_params`**: `eos_token` is also merged into the stop list when present

**Argument order:** the manifest is loaded **after** all CLI flags are parsed, so **manifest values override overlapping CLI settings** (manifest preferred).

```bash
./hal-genai-example-v2 serve \
  --manifest /path/to/hailo_model_zoo_genai/models/manifests/qwen2-vl/2b \
  --hef ./Qwen2-VL-2B-Instruct.hef \
  --port 9000
```

Use a zoo manifest for Qwen2-VL so `generation_params` and `vlm_params` match the model. See [manifest.example.json](manifest.example.json) for a minimal template.

Open **`http://<device-ip>:<port>/`** (port from `--port` or manifest).

### `/api/config` (VLM)

JSON includes `kind`, `hef`, optional `manifest`, and optional **`vlm`**:

- **`vlm.sdk`**: width, height, features, `bytes_per_frame` from the loaded HEF (HAL `get_vlm_input_layout`)
- **`vlm.decode`**: width/height used to decode and resize uploaded images before inference

## API

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/` | Web UI |
| GET | `/api/config` | JSON: `kind`, `hef`, optional `manifest`, optional `vlm` (`sdk` + `decode`) |
| POST | `/api/chat` | JSON: `text`, optional `image_base64`, optional sampling overrides (`temperature`, `top_p`, `top_k`, `max_tokens`, `frequency_penalty`, `do_sample`, `seed`); response `text/event-stream` (`data: {"token":"..."}` … `data: [DONE]`) |
| POST | `/api/chat/abort` | Abort current generation |
| POST | `/api/session/reset` | HAL `clear_context` + clear server-side message list |

Do not use **Clear context** while a stream is still active; use **Stop** first if needed.

## Behaviour notes

- **LLM:** the server keeps a **multi-turn** `messages_json` list so HAL continuation matches the growing history.
- **VLM:** HAL runs **`clear_context()`** on every generate; the example therefore **clears server-side VLM history on each `/api/chat`** so message placeholders always match the current image frame count. Each send is one independent turn (no VLM chat memory across requests).
- **Stop tokens:** loaded from manifest (and enriched for Qwen-style EOS where applicable), applied through HAL; the HAL layer **re-applies** cached stop strings after `clear_context()` so EOS still works on every VLM turn.
- **HailoRT GenAI** rejects **`frequency_penalty == 0`**. HAL skips `set_frequency_penalty` when the value is zero so the SDK default applies. The web UI only sends `text` / `image_base64` by default; sampling defaults come from the manifest or built-in defaults.
- **Special tokens:** common chat EOS strings are stripped from streamed tokens and from stored assistant lines where possible (best-effort per chunk).
- Assistant lines appended server-side use the same JSON escaping as the HAL LLM path so multi-turn **LLM** history stays consistent with `hailo15_genai_impl.cpp`.
