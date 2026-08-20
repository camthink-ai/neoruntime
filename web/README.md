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

Prefer `web/.env` (repo includes a template). You can also create `web/.env.local` to override local settings.

```bash
VITE_API_TARGET=http://127.0.0.1:8080
VITE_APP_BASE=/
```

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
