/**
 * Release notes generator. Roadmap §0.5.
 *
 *   node tools/dist/src/release-notes.js <from-ref>..<to-ref>
 *
 * Aggregates the METRICS: block of every commit in the range into a trend
 * table. The roadmap calls this the project's biggest engineering asset, and
 * the reason is that it turns the commit history into a time series: when the
 * system starts eating 12 kB more RAM six months from now, the answer is a
 * bisect over numbers rather than an archaeology dig through diffs.
 */
import { execFileSync } from 'node:child_process';
import { parseCommitMetricsBlock } from './parse-metrics.js';

export interface CommitMetrics {
  sha: string;
  subject: string;
  metrics: Record<string, number>;
}

const RECORD_SEPARATOR = '@@COMMIT@@';

/** Reads commits in `range`, newest first, with their parsed METRICS blocks. */
export function collectCommitMetrics(range: string, cwd?: string): CommitMetrics[] {
  const raw = execFileSync(
    'git',
    ['log', range, `--pretty=format:${RECORD_SEPARATOR}%H%n%s%n%b`],
    { encoding: 'utf8', cwd: cwd ?? process.cwd(), maxBuffer: 32 * 1024 * 1024 },
  );

  const out: CommitMetrics[] = [];
  for (const record of raw.split(RECORD_SEPARATOR)) {
    if (record.trim() === '') continue;
    const lines = record.split('\n');
    const sha = lines[0]?.trim() ?? '';
    const subject = lines[1]?.trim() ?? '';
    if (sha === '') continue;
    out.push({ sha, subject, metrics: parseCommitMetricsBlock(lines.slice(2).join('\n')) });
  }
  return out;
}

/**
 * Renders the trend table.
 *
 * Commits are listed oldest-first so a column reads left to right as the metric
 * actually evolved. A metric that appears in only one commit still gets a row —
 * that is the point at which it entered the history.
 */
export function renderReleaseNotes(commits: CommitMetrics[], tag: string): string {
  const chronological = [...commits].reverse();
  const withMetrics = chronological.filter((c) => Object.keys(c.metrics).length > 0);

  const lines: string[] = [`# ${tag}`, ''];

  lines.push('## Commits', '');
  for (const commit of chronological) {
    lines.push(`- \`${commit.sha.slice(0, 7)}\` ${commit.subject}`);
  }
  lines.push('');

  const names = [...new Set(withMetrics.flatMap((c) => Object.keys(c.metrics)))].sort();
  if (names.length === 0) {
    lines.push('_No METRICS: blocks found in this range._');
    return lines.join('\n');
  }

  lines.push('## Metric trend', '');
  lines.push(`| Metric | ${withMetrics.map((c) => c.sha.slice(0, 7)).join(' | ')} | Δ |`);
  lines.push(`|---|${withMetrics.map(() => '---').join('|')}|---|`);

  for (const name of names) {
    const cells = withMetrics.map((commit) => {
      const value = commit.metrics[name];
      return value === undefined ? '·' : String(value);
    });

    const seen = withMetrics.flatMap((c) => (c.metrics[name] === undefined ? [] : [c.metrics[name]!]));
    const first = seen[0];
    const last = seen[seen.length - 1];
    let delta = '—';
    if (first !== undefined && last !== undefined && seen.length > 1 && first !== 0) {
      const pct = ((last - first) / Math.abs(first)) * 100;
      delta = `${pct >= 0 ? '+' : ''}${pct.toFixed(1)}%`;
    }

    lines.push(`| \`${name}\` | ${cells.join(' | ')} | ${delta} |`);
  }

  lines.push('');
  lines.push(
    `_${names.length} metrics across ${withMetrics.length} of ${chronological.length} commits._`,
  );
  return lines.join('\n');
}
