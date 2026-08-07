# AIPC Nginx App Gateway

This directory contains the nginx gateway assets that are packaged into the
AIPC release under `/data/aipc/nginx` and installed onto the device nginx root
under `/data/nginx`.

The gateway exposes app web UIs through stable URLs:

```text
https://<device>/apps/<app-id>/
```

`aipc-nginx-app-route-sync.py` watches the app registry and app manifests. For
each running host-network app with `spec.permissions.network.inbound`, it
generates nginx include files and reloads nginx when the route set changes.

Generated include files are deliberately separate from `nginx.conf`:

- `app-routes.conf`: `/apps/<app-id>/` route definitions.
- `app-root-upstreams.conf`: referer-based upstream map entries for root-path
  app requests such as `/static/...` or `/api/...`.
- `app-root-prefixes.conf`: referer-based `X-Forwarded-Prefix` values.

Files ending in `.seed` are only used to initialize missing generated include
files during install. They should stay generic and contain no app-specific
state.

`runtime/` carries the Hailo-15 nginx userspace required by the gateway service:

- `runtime/bin/nginx`
- `runtime/rootfs/usr/sbin/nginx`
- `runtime/rootfs/lib/aarch64-linux-gnu/libcrypt.so.1*`
- `runtime/rootfs/usr/lib/aarch64-linux-gnu/libpcre2-8.so.0*`

Do not add `/data/nginx/logs`, old config backups, `run`, or `tmp` content here.
