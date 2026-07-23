import { ComponentManifest, ManifestParam } from '@lorahome/components';
import { useState } from 'react';

export interface ManifestFormProps {
  manifest: ComponentManifest;
  onChange?: (values: Record<string, unknown>) => void;
}

function defaultValues(manifest: ComponentManifest): Record<string, unknown> {
  return Object.fromEntries(manifest.params.map((p) => [p.key, p.default]));
}

/**
 * Renders a configuration form purely from a component manifest — adding a
 * new sensor should never require touching this file. See CONTRIBUTING.md
 * §2: if a manifest doesn't render correctly, that's a bug here, not a
 * reason to special-case a component by id.
 */
export function ManifestForm({ manifest, onChange }: ManifestFormProps) {
  const [values, setValues] = useState<Record<string, unknown>>(() => defaultValues(manifest));

  function setField(key: string, value: unknown) {
    const next = { ...values, [key]: value };
    setValues(next);
    onChange?.(next);
  }

  return (
    <form className="manifest-form">
      <h3>{manifest.id}</h3>
      {manifest.params.map((param) => (
        <ManifestFormField key={param.key} param={param} value={values[param.key]} onChange={(v) => setField(param.key, v)} />
      ))}
    </form>
  );
}

function ManifestFormField({
  param,
  value,
  onChange,
}: {
  param: ManifestParam;
  value: unknown;
  onChange: (value: unknown) => void;
}) {
  const label = param.key;

  switch (param.type) {
    case 'enum':
      return (
        <label>
          {label}
          <select value={String(value)} onChange={(e) => onChange(e.target.value)}>
            {(param.options ?? []).map((opt) => (
              <option key={String(opt)} value={String(opt)}>
                {String(opt)}
              </option>
            ))}
          </select>
        </label>
      );
    case 'bool':
      return (
        <label>
          {label}
          <input type="checkbox" checked={Boolean(value)} onChange={(e) => onChange(e.target.checked)} />
        </label>
      );
    case 'uint8':
    case 'uint16':
    case 'uint32':
    case 'int32':
    case 'float':
      return (
        <label>
          {label}
          <input
            type="number"
            value={Number(value)}
            min={param.min}
            max={param.max}
            onChange={(e) => onChange(Number(e.target.value))}
          />
        </label>
      );
    default:
      return (
        <label>
          {label}
          <input type="text" value={String(value)} onChange={(e) => onChange(e.target.value)} />
        </label>
      );
  }
}
