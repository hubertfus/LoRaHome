import assert from 'node:assert/strict';
import { test } from 'node:test';
import { renderReleaseNotes, type CommitMetrics } from '../src/release-notes.js';

const COMMITS: CommitMetrics[] = [
  // Newest first, as `git log` emits them.
  { sha: 'ccccccc111', subject: 'feat: third', metrics: { 'bench.parse': 80, 'size.text': 200 } },
  { sha: 'bbbbbbb222', subject: 'feat: second', metrics: { 'bench.parse': 90 } },
  { sha: 'aaaaaaa333', subject: 'feat: first', metrics: { 'bench.parse': 100, 'size.text': 180 } },
];

test('lists commits oldest-first so a row reads as history', () => {
  const notes = renderReleaseNotes(COMMITS, 'v0.1.0-alpha');
  const first = notes.indexOf('feat: first');
  const third = notes.indexOf('feat: third');
  assert.ok(first < third, 'oldest commit should appear first');
});

test('renders a row per metric with a delta across the range', () => {
  const notes = renderReleaseNotes(COMMITS, 'v0.1.0-alpha');
  assert.match(notes, /# v0\.1\.0-alpha/);
  // bench.parse went 100 -> 80 across the etap: a 20% improvement.
  assert.match(notes, /\| `bench\.parse` \| 100 \| 90 \| 80 \| -20\.0% \|/);
  // size.text is absent from the middle commit and shows a placeholder.
  assert.match(notes, /\| `size\.text` \| 180 \| · \| 200 \| \+11\.1% \|/);
});

test('a metric seen only once gets no delta rather than a fake one', () => {
  const notes = renderReleaseNotes(
    [{ sha: 'abc1234', subject: 'feat: only', metrics: { 'bench.new': 5 } }],
    'v0.1.0',
  );
  assert.match(notes, /\| `bench\.new` \| 5 \| — \|/);
});

test('a range with no METRICS blocks says so instead of rendering an empty table', () => {
  const notes = renderReleaseNotes([{ sha: 'abc1234', subject: 'chore: x', metrics: {} }], 'v0.1.0');
  assert.match(notes, /No METRICS: blocks found/);
  assert.doesNotMatch(notes, /Metric trend/);
});

test('summarises how much of the range carried metrics', () => {
  const notes = renderReleaseNotes(COMMITS, 'v0.1.0-alpha');
  assert.match(notes, /2 metrics across 3 of 3 commits/);
});
