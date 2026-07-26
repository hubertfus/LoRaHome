/**
 * The full reliability run: 100k frames through each of five link profiles (T2.6).
 *
 * The unit test in test/chaos.test.ts runs the same harness at 2k frames and
 * guards the invariant on every pull request. This is the measured run — the
 * one whose delivery rates go into the commit message and into the metric
 * history, where a change in the ARQ six months from now shows up as a number
 * that moved rather than as a support call.
 *
 * Runtime is a metric of its own. The roadmap's 180 s ceiling is not comfort:
 * a suite that takes ten minutes gets moved to nightly, and a suite that runs
 * nightly stops preventing bad merges and starts documenting them.
 */
import { deliveryPct, runChaos, type ChaosResult } from '../src/reliability/chaos.js';
import { LINK_PROFILES, type LinkProfileName } from '../src/transport/sim-link.js';

/** Overridable so a developer can sanity-check the wiring in a few seconds. */
const FRAMES = Number(process.env.LH_CHAOS_FRAMES ?? 100_000);

/**
 * One seed for the whole suite, printed with every result.
 *
 * A failure has to be reproducible from what is on the screen: profile plus
 * seed plus frame count is enough to re-run it exactly (R2.6).
 */
const SEED = Number(process.env.LH_CHAOS_SEED ?? 0x8f2a);

const PROFILES = Object.keys(LINK_PROFILES) as LinkProfileName[];

console.log(`LH_ENV chaos.frames=${FRAMES}`);
console.log(`LH_ENV chaos.seed=0x${SEED.toString(16)}`);

const started = process.hrtime.bigint();
const results: ChaosResult[] = [];
let totalDoubleProcessed = 0;

for (const profile of PROFILES) {
  const result = runChaos({ profile, frames: FRAMES, seed: SEED });
  results.push(result);
  totalDoubleProcessed += result.doubleProcessed;

  const pct = deliveryPct(result);
  // Three decimals because the budgets are written in them: 99.99% and 99.9%
  // are different answers, and rounding to two would make them the same.
  console.log(
    `LH_METRIC chaos.${profile}.delivery value=${pct.toFixed(3)} unit=pct` +
      ` (retries: ${result.retries}, crc_rejects: ${result.crcRejects}, give_ups: ${result.giveUps})`,
  );
  console.log(`LH_METRIC chaos.${profile}.retries value=${result.retries} unit=count`);
  console.log(`LH_METRIC chaos.${profile}.crc_rejects value=${result.crcRejects} unit=count`);
  console.log(`LH_METRIC chaos.${profile}.dupes_dropped value=${result.dupesDropped} unit=count`);
  console.log(
    `LH_METRIC chaos.${profile}.runtime.s value=${(result.runtimeMs / 1000).toFixed(2)} unit=s`,
  );
}

/**
 * The same run with reliable acknowledgements.
 *
 * Published delivery figures — this project's roadmap included — are quoted
 * under this model, where the sender's frames can be lost but ACKs always
 * return. Measured separately rather than substituted, because a number
 * compared against a budget from a different model is worse than no number.
 */
const uplinkOnly = runChaos({
  profile: 'loss30',
  frames: FRAMES,
  seed: SEED,
  lossyAcks: false,
});
totalDoubleProcessed += uplinkOnly.doubleProcessed;
console.log(
  `LH_METRIC chaos.loss30.delivery.uplink_only value=${deliveryPct(uplinkOnly).toFixed(3)} unit=pct` +
    ` (retries: ${uplinkOnly.retries})`,
);

const runtimeSeconds = Number(process.hrtime.bigint() - started) / 1e9;

// The invariant, last and on its own line. Zero is the only passing value, and
// the budget makes that the gate's business rather than a reader's.
console.log(`LH_METRIC chaos.double_processed value=${totalDoubleProcessed} unit=count budget=0`);
console.log(
  `LH_METRIC chaos.suite.runtime.s value=${runtimeSeconds.toFixed(1)} unit=s budget=180`,
);
console.log(
  `LH_METRIC chaos.frames_per_profile value=${FRAMES} unit=count budget=${FRAMES}`,
);

const worst = results.reduce((low, result) =>
  deliveryPct(result) < deliveryPct(low) ? result : low,
);
console.log(
  `\n${results.length} profiles, ${FRAMES} frames each, seed 0x${SEED.toString(16)} — ` +
    `worst delivery ${deliveryPct(worst).toFixed(3)}% on ${worst.profile}, ` +
    `${totalDoubleProcessed} double-processed`,
);

if (totalDoubleProcessed > 0) {
  console.error('\nINVARIANT BROKEN: a frame was processed more than once.');
  process.exit(1);
}
