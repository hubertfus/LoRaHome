/**
 * LoRa time-on-air estimate, per Semtech AN1200.22.
 * Used by the Duty Cycle Guard to enforce the ETSI EN 300 220 1% limit on 868MHz.
 */
export interface AirtimeParams {
  /** Payload size in bytes (header + CBOR body, excluding CRC is fine — CRC is small and constant). */
  bytes: number;
  /** Spreading factor, 7-12. */
  spreadingFactor: number;
  /** Bandwidth in Hz, e.g. 125000. */
  bandwidthHz: number;
  /** Coding rate denominator offset: 1 = 4/5 ... 4 = 4/8. Default 1. */
  codingRate?: number;
  /** Explicit header enabled. Default true. */
  explicitHeader?: boolean;
  /** Low data rate optimization. Default: auto-enabled for SF11/SF12 @ 125kHz. */
  lowDataRateOptimize?: boolean;
  preambleSymbols?: number;
}

export function computeAirtimeMs(params: AirtimeParams): number {
  const {
    bytes,
    spreadingFactor: sf,
    bandwidthHz: bw,
    codingRate: cr = 1,
    explicitHeader = true,
    preambleSymbols = 8,
  } = params;

  const lowDataRateOptimize = params.lowDataRateOptimize ?? (sf >= 11 && bw <= 125000);
  const de = lowDataRateOptimize ? 1 : 0;
  const h = explicitHeader ? 0 : 1;

  const symbolDurationMs = (2 ** sf / bw) * 1000;

  const numerator = 8 * bytes - 4 * sf + 28 + 16 - 20 * h;
  const denominator = 4 * (sf - 2 * de);
  const payloadSymbNb = 8 + Math.max(Math.ceil(numerator / denominator) * (cr + 4), 0);

  const preambleDurationMs = (preambleSymbols + 4.25) * symbolDurationMs;
  const payloadDurationMs = payloadSymbNb * symbolDurationMs;

  return preambleDurationMs + payloadDurationMs;
}
