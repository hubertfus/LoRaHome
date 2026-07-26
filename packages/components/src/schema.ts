/**
 * Browser-safe entry point: the manifest schema and its validators, nothing else.
 *
 * Separate from `index.ts` because that barrel also re-exports the filesystem
 * loader, and a bundler following it pulls `node:fs` into a web build. The web
 * package imports from here.
 */
export * from './manifest-schema.js';
