/**
 * `pnpm gen:c`         — write firmware/common/include/lorahome/protocol_generated.h
 * `pnpm gen:c --check` — verify the file on disk matches what the schema emits
 *
 * On the drift gate: T0.3 specifies `git diff --exit-code` for this, but R0.3
 * also puts the generated header in .gitignore, and an ignored file never shows
 * up in a diff — the two instructions cannot both hold. Keeping it untracked is
 * the right call (generated artefacts do not belong in history), so the check
 * compares emitted bytes against the file on disk instead. That is strictly
 * stronger than the git version: it catches hand edits whether or not the file
 * is tracked, and it works in a fresh clone where the file does not exist yet.
 */
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { countStaticAsserts, emitCHeader, generatedHeaderHash } from './emit-c.js';

/** Walks up from this module to the repository root (the dir holding pnpm-workspace.yaml). */
function repoRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  for (;;) {
    try {
      readFileSync(join(dir, 'pnpm-workspace.yaml'));
      return dir;
    } catch {
      const parent = dirname(dir);
      if (parent === dir) throw new Error('could not locate repo root (no pnpm-workspace.yaml)');
      dir = parent;
    }
  }
}

export const OUTPUT_RELATIVE = 'firmware/common/include/lorahome/protocol_generated.h';

function main(): void {
  const checkOnly = process.argv.includes('--check');
  const outputPath = resolve(repoRoot(), OUTPUT_RELATIVE);
  const started = process.hrtime.bigint();
  const source = emitCHeader();
  const elapsedMs = Number(process.hrtime.bigint() - started) / 1e6;

  if (checkOnly) {
    let onDisk: string;
    try {
      onDisk = readFileSync(outputPath, 'utf8');
    } catch {
      console.error(`DRIFT: ${OUTPUT_RELATIVE} is missing. Run \`pnpm gen:c\`.`);
      process.exit(1);
    }

    if (onDisk !== source) {
      console.error(
        `DRIFT: ${OUTPUT_RELATIVE} does not match the schema.\n` +
          'Someone edited the generated file by hand, or changed frame.ts without\n' +
          'regenerating. Run `pnpm gen:c` and commit the result.',
      );
      process.exit(1);
    }
    console.log(`gen:c --check OK (${OUTPUT_RELATIVE} matches schema ${generatedHeaderHash()})`);
    return;
  }

  mkdirSync(dirname(outputPath), { recursive: true });
  writeFileSync(outputPath, source, 'utf8');

  const bytes = Buffer.byteLength(source, 'utf8');
  console.log(`wrote ${relative(repoRoot(), outputPath).replace(/\\/g, '/')}`);
  console.log(`LH_METRIC bench.codegen.ms value=${elapsedMs.toFixed(1)} unit=ms budget=500`);
  console.log(`LH_METRIC size.generated_h value=${bytes} unit=B`);
  console.log(`LH_METRIC count.static_asserts value=${countStaticAsserts(source)} unit=count`);
  console.log(`LH_METRIC codegen.source_hash value=${generatedHeaderHash()} unit=hash`);

  if (elapsedMs > 500) {
    console.error(`BUDGET BREACH: bench.codegen.ms = ${elapsedMs.toFixed(1)} ms (budget: 500 ms)`);
    process.exit(1);
  }
}

/**
 * Only run when invoked as a script. The test suite imports OUTPUT_RELATIVE
 * from this module, and a bare `main()` call would make merely importing it
 * rewrite a tracked build artefact as a side effect.
 */
const invokedPath = process.argv[1];
if (invokedPath !== undefined && resolve(invokedPath) === fileURLToPath(import.meta.url)) {
  main();
}
