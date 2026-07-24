/**
 * CLI for the release-notes generator.
 *
 *   node tools/dist/src/release-notes-cli.js <range> [--tag v0.1.0-alpha] [--out FILE]
 *
 * Example: release-notes-cli.js b3b3eef..HEAD --tag v0.1.0-alpha
 */
import { writeFileSync } from 'node:fs';
import { collectCommitMetrics, renderReleaseNotes } from './release-notes.js';

function argValue(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  return index === -1 ? undefined : process.argv[index + 1];
}

const range = process.argv[2];
if (range === undefined || range.startsWith('--')) {
  console.error('usage: release-notes-cli <from>..<to> [--tag NAME] [--out FILE]');
  process.exit(2);
}

const notes = renderReleaseNotes(collectCommitMetrics(range), argValue('--tag') ?? range);
const outPath = argValue('--out');

if (outPath === undefined) {
  console.log(notes);
} else {
  writeFileSync(outPath, notes + '\n', 'utf8');
  console.log(`wrote ${outPath}`);
}
