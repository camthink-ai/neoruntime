## NeoRuntime AI IPC

---

## Dev Requirements

- Node.js: recommended **18+ / 20+**
- Package manager: **pnpm**

---

## Quick Start

```bash
pnpm install
pnpm dev
```

- Dev port: `5174`
- Proxy: requests to `/api/*` are forwarded to `VITE_API_TARGET`

---

## Environment Variables

A committed template lives at [`web/.env.example`](./.env.example). Copy it to a gitignored `web/.env` and adjust the values for your setup:

```bash
cp web/.env.example web/.env
```

You can also create `web/.env.local` to override individual settings without touching `web/.env` (also gitignored).

| Variable | Default | Description |
| --- | --- | --- |
| `VITE_API_TARGET` | `http://localhost:8080` | Backend target for the Vite dev proxy (`/api/*`). |
| `VITE_WS_URL` | — | WebSocket endpoint (LOCAL/REMOTE templates in `.env.example`). |
| `VITE_VIDEO_STREAM_URL` | — | H.264 video stream WebSocket endpoint. |
| `VITE_API_BASE_URL` | `/api/v1` | Relative API base path used by the frontend. |
| `VITE_API_TOKEN` | — | API token, if your backend requires one. |
| `VITE_USE_MOCK_DATA` | `false` | `true` serves mock data instead of calling the API. |
| `VITE_ENABLE_AUTH` | `true` | `false` disables the login/auth flow. |
| `VITE_APP_BASE` | `/` | Public path the app is served from. |
| `VITE_HTTPS` | off | `true` serves dev over HTTPS via a trusted self-signed cert (mkcert). |

See [`web/.env.example`](./.env.example) for the LOCAL/REMOTE backend templates and full comments.

---

## Common Commands

```bash
# Dev (local)
pnpm dev

# Build (tsc -b + vite build)
pnpm build

# Type-check only (no build output)
pnpm exec tsc --noEmit

# Preview the build output
pnpm preview

# Tests (Vitest)
pnpm test
pnpm test:run
pnpm test:ui
pnpm test:coverage

# Lint (ESLint)
pnpm lint
pnpm lint:fix
pnpm lint:check

# Format (Prettier)
pnpm format
pnpm format:check
```

---

## Conventions

- **Routes/Pages**: `src/pages/`
- **Shared components**: `src/components/` (including `src/components/ui/`)
- **API/services**: `src/services/`
- **State**: `src/store/`
- **Styles/themes**: `src/styles/` (entry: `src/styles/index.css`)
- **i18n**: `src/i18n/` (use `t('sys.xxx.yyy', 'default text')` in components)

---

## Notes

- **API dev proxy**: in dev, call `/api/*` and Vite proxies it to `VITE_API_TARGET` (see `vite.config.ts`).
- **Entry files**:
  - `src/main.tsx`: app bootstrap (React Query, Toaster, i18n, global styles, etc.)
  - `src/App.tsx`: top-level providers (theme, tooltip, router)
