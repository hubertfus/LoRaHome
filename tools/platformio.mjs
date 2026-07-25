/**
 * Locates PlatformIO Core.
 *
 * Shared by tools/build-firmware.mjs and tools/run-unit-tests.mjs. Its own
 * module because two copies of "where is pio" is the same drift hazard as two
 * copies of a wire format, just cheaper to fix.
 */
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';

/** The pio executable, or null. Its own venv first, then PATH. */
export function findPlatformIO() {
  const candidates = [
    join(homedir(), '.platformio', 'penv', 'Scripts', 'platformio.exe'),
    join(homedir(), '.platformio', 'penv', 'bin', 'platformio'),
    'platformio',
    'pio',
  ];

  for (const candidate of candidates) {
    if (candidate.includes('/') || candidate.includes('\\')) {
      if (existsSync(candidate)) return candidate;
      continue;
    }
    try {
      execFileSync(candidate, ['--version'], { stdio: 'ignore' });
      return candidate;
    } catch {
      /* not on PATH */
    }
  }
  return null;
}

export function platformioVersion(pio) {
  try {
    return execFileSync(pio, ['--version'], { encoding: 'utf8', stdio: 'pipe' }).trim();
  } catch {
    return 'unknown';
  }
}

/**
 * PATH with a compiler's directory prepended, for PlatformIO's `native` platform.
 *
 * That platform shells out to bare `gcc`/`g++`, so a toolchain discovered by
 * absolute path — MSYS2 keeps its `bin` off the Windows PATH deliberately — is
 * invisible to it unless the directory is put in front. Scoped to the child
 * process rather than written into the machine's environment.
 */
export function environmentWithCompiler(compilerPath) {
  if (compilerPath === null || (!compilerPath.includes('/') && !compilerPath.includes('\\'))) {
    return process.env;
  }
  const currentPath = process.env.PATH ?? process.env.Path ?? '';
  return { ...process.env, PATH: `${dirname(compilerPath)}${delimiter}${currentPath}` };
}
