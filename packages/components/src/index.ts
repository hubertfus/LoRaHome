/**
 * The schema is browser-safe; the loader is not.
 *
 * `registry.ts` reads the filesystem, so anything importing a *runtime* value
 * from this barrel drags `node:fs` into a browser bundle — Vite fails the build
 * with "join is not exported by __vite-browser-external", pointing at a file
 * nobody in the web package meant to use. Type-only imports are erased and were
 * fine, which is why this stayed hidden until the first runtime import.
 *
 * Both are still exported here for Node consumers. Browser code should import
 * from `@lorahome/components/schema` instead, which cannot reach the loader.
 */
export * from './manifest-schema.js';
export * from './registry.js';
