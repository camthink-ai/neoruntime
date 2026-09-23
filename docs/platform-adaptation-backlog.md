# Platform Adaptation Backlog (post DSP P0)

Status: work list — what the platform layers owe next, after the DSP
offload chain (HAL-1..4/7, PLAT-1..6) landed on `develop`
(`ed0485d..248b018`) and the ported daemon was re-verified end to end
on the test device (2026-09-01, 7/7 probe: resize / crop / multi-crop,
keep-fd zero-copy import, mmap + encoded-stream regressions,
`Frame.resize` fast path).

Every item carries its evidence: a `file:line` into this repo, a
commit, or an on-device measurement. Suggested ordering is at the
bottom.

## A. Release hygiene (no code)

| # | Item | Detail |
|---|------|--------|
| A1 | **`develop` is local-only** | The full DSP chain (5 commits + the folded rotation fix) exists only in a local worktree. Push and open the PR — until then the SDK 0.6.0 DSP client has no shippable daemon counterpart. |
| A2 | **Daemon version / capability bump** | `SubmitDspJob` + `DSP_IMPORT` are new contract surface that SDK 0.6.0 already probes by socket/RPC presence. A daemon version bump (and/or a feature-flag RPC) lets the SDK report capabilities cleanly instead of sniffing. |
| A3 | **Keep the ported commits' source lineage in sync** | The same content now lives in two lineages (the port source branch and `develop`). Sync or retire one; two unpushed copies of the same fixes is divergence risk. |

## B. Known daemon defects (identified, not yet fixed)

| # | Defect | Evidence / fix pattern |
|---|--------|------------------------|
| B1 | **`encoded_publisher` blocking send** | The dispatch path still does a blocking `send_all` (platform/camera-daemon/src/encoded_publisher.cpp); the journal shows ~8 ms slow-send warnings under load. Same defect class as fd_publisher #3 fixed in `31ce6fc` — reuse its pattern: `MSG_DONTWAIT` per sendmsg, EAGAIN → drop frame / keep client, partial send → drop client (with SCM_RIGHTS the fds crossed with the first queued byte). |
| B2 | **Audio capture socket carries a video-layout header** | The publisher writes a video `EncHeader` on the audio capture socket; the audio fields are never on the wire (`rate=0`, `ch=0`, `bits=low32(pts)`). The audio path must write a real audio layout header. SDKs were deliberately left unchanged — this is daemon-side. |
| B3 | **MULTI_CROP + NEAREST surfaces as a generic −2801** | Proto default `interpolation=0` is NEAREST, which the vendor batched op rejects (`DSP_INVALID_ARGUMENT` → −2801); only BILINEAR/BICUBIC are accepted. A bare gRPC client that omits `set_interpolation()` fails every multi-crop job with an opaque error. Daemon-side validation should reject NEAREST-on-MULTI_CROP with a named error before it reaches the vendor lib. (The SDK already always sends explicit interpolation.) |
| B4 | *(small)* letterbox pad color is `HalDspColor{}` = Y=U=V=0 | Renders green-tinted in NV12, not a neutral pad. Default to neutral (Y=16, U=V=128) or document the vendor semantics. The SDK already routes around it (scale into a client-computed box, pad on CPU). |

## C. ai-runtime −2814 fix provenance (asset risk)

Root cause (measured on device): ai-runtime submits fd-backed tensors
(`data=nullptr`) while the HAL inference path requires `in.data` →
`HAL_ERR_INVALID_ARG` (−2814); every camera-stream subscription
inference failed until patched. **The deployed fix exists only as a
patched binary on the test device — its source has not been located.**
Rebuilding ai-runtime today silently regresses it.

Action: locate and land the patch (or reimplement from the recorded
root cause above) and add a regression test that submits an fd-backed
tensor through the HAL bridge.

## D. Operational leftovers

| # | Item |
|---|------|
| D1 | app-manager gRPC hung once during device verification; needs a restart plus root cause (hang, not crash — no restart counter movement). |
| D2 | *(optional)* frame-retention budget ~4 s (FrameWatchdog warns ~4.2 s, force-reclaims ~5 s) is fixed; make it configurable if apps need longer holds. |

## E. HAL / firmware capability gaps (SDK already shipped the client side)

| # | Item | Detail |
|---|------|--------|
| E1 | **`set_af_windows` / `get_af_measurement`** | The lens HAL bridge answers "not yet supported" — the only device-verification FAIL of SDK 0.6.0's native AF group. Firmware/bridge work unlocks the API with zero SDK change. |
| E2 | *(optional, HAL-5)* second daemon DSP handle with `dsp_set_priority(HIGH)` | The vendor singleton queue arbitrates per handle: platform jobs (encoder/DPM) submitted behind app jobs would still run first. Cheap — multi-handle was proven functional in the P0 experiments. |
| E3 | *(optional, HAL-6)* expose `dsp_get_utilization` as telemetry | Quota tuning needs a feedback signal; the 19 %/21 % utilization numbers came from a probe redeclaration. Also turns the black-box encoder-contention gate (PLAT-6) white-box. |
| E4 | *(document)* vendor SCALE_AND_CROP rounding diverges from CPU placement by ~21 mean&#124;diff&#124; (16:9→4:3, live 4K) | The SDK avoids vendor letterbox/scale_crop entirely (DSP stretch into a client-computed box). Either escalate to the vendor or document the divergence. |

## F. Daemon contract rollout (proposals ready, dependencies unblocked)

The `dsp-offload` proposal is shipped (PLAT-1..5). Three remain, in
cost/benefit order:

| # | Contract | Content | Cost |
|---|----------|---------|------|
| F1 | **ai-overlay-extended** (cheapest) | `AiOverlayConfig` v2: polygons, tracks, per-class colors, per-app overlay sources. Its blend dependency is already hardware-validated (HAL-2: alpha=0 passthrough exact, ~129 µs/overlay marginal on dma-buf, ~0.67 ms single). | renderer extension only |
| F2 | **frame-injection** | `PushFrame`: app-composed frames / OSD into the encoded main stream. The injection node exists and sits idle on the target device (`/dev/video10`, `hailo-vid-out-mcm-in`, memory-injection). | media-graph wiring + dma-buf import contract + geometry/format constraints + EOS/flush semantics |
| F3 | **web-stream-url** | `GetWebStreamUrl`: apps ask the platform console for a video endpoint (HLS/MJPEG reverse-proxied by the platform) instead of opening their own ports. | RPC + nginx; no dependencies |

## G. Telemetry / quota operations

Expose per-client jobs/s, quota rejections, and DSP utilization via a
status RPC or the event bus. PLAT-6's encoder gate had to be black-box
 precisely because the encoder's DSP usage is invisible to daemon
instrumentation; E3 makes it measurable, and quota anchors
(100 jobs/s, 120 MPix/s per client) become tunable instead of guessed.

## Suggested order

```
A  (release hygiene — otherwise the DSP work is an orphan)
C  (ai-runtime patch provenance — rebuild regression risk)
B1 B2 B3  (known defects, fix patterns already proven)
F1 (cheapest new capability)
E1 (unblocks the only SDK 0.6.0 device FAIL)
F2 F3      (new app-facing surface)
optional tail: B4, D2, E2, E3, E4, G
```
