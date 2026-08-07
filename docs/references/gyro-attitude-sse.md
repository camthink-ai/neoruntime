# Gyro Attitude SSE Endpoint Contract (Horizontal Calibration)

> Contract for implementing `GET /api/v1/monitor/gyro/attitude`
> (`text/event-stream`).
> Frontend consumers: `web/src/pages/dashboard/components/GyroCalibrationCard.tsx`
> (parsed via `web/src/lib/gyroStream/gyroSSE.ts`).

## 1. Background

The horizontal-calibration card only cares about "how far the device is from
level" — it does not care about orientation (yaw). Attitude data is therefore
expressed **only as two tilt angles, with no Z-axis (yaw)**. The backend should
fuse the IMU data and project the attitude into two angles, and the frontend
rotates a visualization plate accordingly (front/back and left/right tilt only,
never rotating around the Z axis).

## 2. Coordinate System and Angle Conventions

The world frame is **Z-up** (+Z upward), matching the sensor bonded to the top
surface of the device:

| Axis | Direction |
|------|-----------|
| +X | Device front |
| -X | Device rear |
| +Y | Device right |
| -Y | Device left |
| +Z | Up |

The two angles:

| Field | Meaning | Positive direction | Range |
|-------|---------|--------------------|-------|
| `pitch` | Front/back tilt (°) | Positive = nose-down (+X side sinking) | [-90, 90] |
| `roll`  | Left/right tilt (°) | Positive = right-down (+Y side sinking) | [-90, 90] |

> Derivation: `pitch = atan2(up.x, up.z)`, `roll = atan2(up.y, up.z)`, where `up`
> is the device top-surface normal in world coordinates. The yaw component is
> discarded outright (never sent).

Level detection (frontend): combined tilt = `acos(up.z)`; ≤ 2° is treated as
"level".

## 3. Endpoint

- **Method / Path**: `GET /api/v1/monitor/gyro/attitude`
- **Content-Type**: `text/event-stream`
- **Authentication**: `EventSource` cannot set custom headers, so the token is
  carried as a query parameter (same convention as the audio-intercom
  WebSocket).

### 3.1 Query Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `token` | string | Yes | — | Auth token (raw value, no `Bearer ` prefix) |
| `rate`  | int    | No  | 50  | Output frequency cap (Hz), clamped to [1, 200] by the backend |

> The legacy `format=quaternion|euler` parameter is deprecated; the backend no
> longer needs to support it.

### 3.2 SSE Events

#### `orientation` — attitude frame (core)

Each frame payload is JSON:

```json
{ "pitch": 12.34, "roll": -5.67 }
```

- `pitch` and `roll` are both numbers (float degrees).
- Must be finite (no `NaN`/`Infinity`/`null`); non-numeric frames are silently
  dropped by the frontend.
- The publish rate should not exceed `rate`; throttle down when the device is
  steady to save bandwidth.

Raw SSE line example:

```
event: orientation
data: {"pitch":12.34,"roll":-5.67}

```

#### `status` — sensor health

```json
{ "sensor": "online" }
```

`sensor` values: `online` / `offline` / `error`.

#### `error` — sensor error details

Sent after a non-`online` status, carrying a readable error message (free text).

#### `heartbeat` — keep-alive

One empty event every 15s to keep the connection from being dropped by proxies.

## 4. Connection Lifecycle

- When the sensor is momentarily unavailable, **do not close the stream**;
  emit `status`/`error` events instead, so the frontend can show the state
  without reconnecting.
- A `503` (non-SSE) is returned only when the gyro source is disabled on the
  server. In that case `EventSource` transitions to `CLOSED` and fires
  `onError`, and the frontend falls back to a local mock (DEV demo only).
- Transient network errors are auto-recovered by `EventSource`; `onError` fires
  but the connection stays alive.

## 5. Frontend Parsing Logic (for backend reference)

```ts
es.addEventListener('orientation', (e) => {
  const d = JSON.parse(e.data);
  const { pitch, roll } = d;
  if (typeof pitch === 'number' && typeof roll === 'number'
      && Number.isFinite(pitch) && Number.isFinite(roll)) {
    onAttitude({ pitch, roll });
  }
});
```

## 6. Differences from the Legacy (Quaternion) Version

| Item | Legacy | New |
|------|--------|-----|
| Attitude representation | Quaternion `[x,y,z,w]` (includes yaw) | Two angles `{pitch, roll}` (no yaw) |
| `orientation` payload | `{ "quaternion": [x,y,z,w] }` | `{ "pitch": <deg>, "roll": <deg> }` |
| Query `format` | `quaternion`/`euler` | Deprecated |
| Visualization | Plate can rotate around Z | Plate tilts on front/back and left/right axes only; yaw locked |
| HUD readout | Derived from quaternion | Shows received pitch/roll directly |

## 7. Self-Test Checklist

- Device flat → `pitch≈0, roll≈0`, frontend shows "level".
- Front edge down 10° → `pitch≈+10`.
- Right edge down 10° → `roll≈+10`.
- Rotating to any heading (around Z) → `pitch`/`roll` must not change (yaw
  stripped).
