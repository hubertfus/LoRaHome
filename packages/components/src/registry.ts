import { readdir, readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ComponentManifest, validateManifest } from './manifest-schema.js';

// Compiled output lives at dist/src/registry.js; manifests/ sits at the package root.
const DEFAULT_MANIFESTS_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'manifests');

/** Loads and validates every *.json manifest in a directory (default: ../manifests). */
export async function loadManifests(dir: string = DEFAULT_MANIFESTS_DIR): Promise<ComponentManifest[]> {
  const files = (await readdir(dir)).filter((f) => f.endsWith('.json'));
  const manifests: ComponentManifest[] = [];
  for (const file of files) {
    const raw = await readFile(join(dir, file), 'utf8');
    manifests.push(validateManifest(JSON.parse(raw)));
  }
  return manifests;
}
