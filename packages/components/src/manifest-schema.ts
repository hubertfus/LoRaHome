/** Component manifest schema. See ARCHITECTURE.md §5. Manifests never go over the radio as-is. */

export type Bus = 'i2c' | 'spi' | 'gpio' | 'onewire' | 'uart';

export type ParamType = 'uint8' | 'uint16' | 'uint32' | 'int32' | 'float' | 'bool' | 'enum' | 'string';

export interface ManifestParam {
  key: string;
  type: ParamType;
  default: unknown;
  options?: unknown[]; // required when type === 'enum'
  min?: number;
  max?: number;
}

export type OutputType = 'float' | 'uint32' | 'int32' | 'bool';

export interface ManifestOutput {
  id: string;
  unit: string;
  type: OutputType;
}

export interface ManifestPower {
  /** Microamps while actively measuring. */
  active_ua: number;
  /** Microamps while idle/sleeping. */
  sleep_ua: number;
  /** Milliseconds a single measurement takes (excludes warm-up). */
  measurement_ms: number;
}

export interface ComponentManifest {
  id: string;
  bus: Bus;
  addresses?: string[];
  params: ManifestParam[];
  outputs: ManifestOutput[];
  power: ManifestPower;
}

export class ManifestValidationError extends Error {}

/** Structural validation only — not a full JSON Schema implementation. */
export function validateManifest(value: unknown): ComponentManifest {
  if (typeof value !== 'object' || value === null) {
    throw new ManifestValidationError('manifest must be an object');
  }
  const m = value as Record<string, unknown>;

  if (typeof m.id !== 'string' || m.id.length === 0) {
    throw new ManifestValidationError('manifest.id must be a non-empty string');
  }
  if (typeof m.bus !== 'string') {
    throw new ManifestValidationError(`manifest "${m.id}": bus must be a string`);
  }
  if (!Array.isArray(m.params)) {
    throw new ManifestValidationError(`manifest "${m.id}": params must be an array`);
  }
  if (!Array.isArray(m.outputs) || m.outputs.length === 0) {
    throw new ManifestValidationError(`manifest "${m.id}": outputs must be a non-empty array`);
  }
  for (const param of m.params as Record<string, unknown>[]) {
    if (param.type === 'enum' && !Array.isArray(param.options)) {
      throw new ManifestValidationError(`manifest "${m.id}": enum param "${param.key}" requires options[]`);
    }
  }
  const power = m.power as Record<string, unknown> | undefined;
  if (!power || typeof power.active_ua !== 'number' || typeof power.sleep_ua !== 'number') {
    throw new ManifestValidationError(`manifest "${m.id}": power.active_ua / power.sleep_ua must be numbers`);
  }

  return m as unknown as ComponentManifest;
}
