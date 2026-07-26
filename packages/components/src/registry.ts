import { readdir, readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ComponentManifest, validateManifest, validateManifestSet } from './manifest-schema.js';

// Compiled output lives at dist/src/registry.js; manifests/ sits at the package root.
const DEFAULT_MANIFESTS_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'manifests');

/** Where the bundled manifests live, for tools that read them directly. */
export function manifestsDir(): string {
  return DEFAULT_MANIFESTS_DIR;
}

/**
 * Loads and validates every *.json manifest in a directory.
 *
 * The set is validated as well as each file, because a reused `driver_type_id`
 * makes two individually-valid manifests into a broken pair — and that pair is
 * exactly risk R3.4, a host rendering one sensor's name over another's data.
 * Loading is where that has to be caught; nothing downstream would notice.
 */
export async function loadManifests(
  dir: string = DEFAULT_MANIFESTS_DIR,
): Promise<ComponentManifest[]> {
  const files = (await readdir(dir)).filter((f) => f.endsWith('.json')).sort();
  const manifests: ComponentManifest[] = [];
  for (const file of files) {
    const raw = await readFile(join(dir, file), 'utf8');
    try {
      manifests.push(validateManifest(JSON.parse(raw)));
    } catch (error) {
      // Name the file. A validation message about "channel 2" is useless when
      // fifty manifests are being loaded and the error does not say which.
      throw new Error(`${file}: ${(error as Error).message}`, { cause: error });
    }
  }
  validateManifestSet(manifests);
  return manifests;
}
