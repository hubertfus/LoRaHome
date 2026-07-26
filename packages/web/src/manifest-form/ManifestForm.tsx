// From the browser-safe entry point: the package index also re-exports a
// filesystem loader, and importing a runtime value through it pulls node:fs
// into the bundle.
import { BUS_TYPE_IDS, ComponentManifest, ManifestChannel } from '@lorahome/components/schema';

export interface ManifestFormProps {
  manifest: ComponentManifest;
}

/**
 * Renders a component purely from its manifest — adding a new sensor must never
 * require touching this file. See CONTRIBUTING.md §2: if a manifest does not
 * render correctly, that is a bug here, not a reason to special-case a
 * component by id.
 *
 * Manifest schema v1 (T3.6) has no `params` block, so there is nothing editable
 * to render yet: what a component *reports* is fixed by its driver, and what it
 * can be *told* is defined by the config engine in Etap 4. Until then this
 * shows the contract — channels, units, scaling, plausible ranges and the power
 * figures the energy calculator reads — which is the part that exists.
 *
 * The earlier version of this file rendered an editable form from a `params`
 * array that the scaffold schema had and v1 does not. It is not being kept
 * alive against a shape nothing produces.
 */
export function ManifestForm({ manifest }: ManifestFormProps) {
  const addresses = manifest.bus.addresses;

  return (
    <section className="manifest-form">
      <header>
        <h3>{manifest.display_name}</h3>
        <p className="manifest-form__id">
          <code>{manifest.id}</code> · type {manifest.driver_type_id} ·{' '}
          {manifest.bus.type.toUpperCase()}
          {addresses !== undefined && addresses.length > 0 ? ` · ${addresses.join(', ')}` : ''}
        </p>
      </header>

      <table className="manifest-form__channels">
        <thead>
          <tr>
            <th>#</th>
            <th>channel</th>
            <th>unit</th>
            <th>range</th>
          </tr>
        </thead>
        <tbody>
          {manifest.channels.map((channel) => (
            <ChannelRow key={channel.id} channel={channel} />
          ))}
        </tbody>
      </table>

      <dl className="manifest-form__power">
        <dt>active</dt>
        <dd>{manifest.power.active_ma} mA</dd>
        <dt>sleep</dt>
        <dd>{manifest.power.sleep_ua} µA</dd>
        <dt>measurement</dt>
        <dd>{manifest.power.measure_ms} ms</dd>
        <dt>warm-up</dt>
        <dd>{manifest.warmup_ms} ms</dd>
        <dt>minimum interval</dt>
        <dd>{manifest.min_interval_ms} ms</dd>
      </dl>
    </section>
  );
}

/**
 * One channel, with its range shown in display units.
 *
 * The manifest's range bounds the raw integer reading, because that is what the
 * rule engine compares; multiplying by `scale` here is the only place the two
 * views meet, and doing it once is what keeps a float out of everything below.
 */
function ChannelRow({ channel }: { channel: ManifestChannel }) {
  const [low, high] = channel.range;
  const format = (raw: number) => Number((raw * channel.scale).toFixed(3)).toString();

  return (
    <tr>
      <td>{channel.index}</td>
      <td>{channel.id}</td>
      <td>{channel.unit}</td>
      <td>
        {format(low)} – {format(high)}
      </td>
    </tr>
  );
}

/** Wire value for this manifest's bus, as capability discovery reports it. */
export function busWireId(manifest: ComponentManifest): number {
  return BUS_TYPE_IDS[manifest.bus.type];
}
