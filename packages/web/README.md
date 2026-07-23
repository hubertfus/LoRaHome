# @lorahome/web

React + React Flow. UI is generated almost entirely from the manifests in `packages/components` — see `manifest-form/`.

- `editor/` — visual rule graph editor (React Flow).
- `manifest-form/` — engine that renders configuration forms from JSON manifests (don't edit per-component, see [CONTRIBUTING.md](../../CONTRIBUTING.md) §2).
- `simulator/` — dry-runs rule graph paths without hardware.
- `time-travel/` — time slider for replaying sensor and rule-engine state history.
