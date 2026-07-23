export interface TimeTravelSliderProps {
  minMs: number;
  maxMs: number;
  valueMs: number;
  onChange: (atMs: number) => void;
}

/** Scrubs through recorded sensor/rule-engine history. See ARCHITECTURE.md killer feature #6. */
export function TimeTravelSlider({ minMs, maxMs, valueMs, onChange }: TimeTravelSliderProps) {
  return (
    <div className="time-travel-slider">
      <input
        type="range"
        min={minMs}
        max={maxMs}
        value={valueMs}
        onChange={(e) => onChange(Number(e.target.value))}
        style={{ width: '100%' }}
      />
      <span>{new Date(valueMs).toISOString()}</span>
    </div>
  );
}
