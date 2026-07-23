# Contributing to LoRaHome

Thanks for your interest in the project. Before you start — this document describes hard rules, not style suggestions. Some of them (especially around ESP32 firmware) come from memory constraints and timing determinism, not code aesthetics, and **won't be negotiated in code review** without very strong justification.

---

## 1. Firmware rules (ESP32, C/C++)

### 1.1 No dynamic allocation

**No `malloc`, `new`, `std::vector`, `std::string`, `std::map` in code running on the Node or Bridge.** No exceptions in the critical path (frame parsing, rule evaluation, sensor handling).

Why: a Node runs for weeks/months on battery, without a restart. Heap fragmentation on a device with ~300KB RAM is a matter of "when," not "if," it crashes. Memory determinism beats coding convenience.

Instead:

```cpp
// BAD
std::vector<Rule> rules;
rules.push_back(newRule);

// GOOD
#define MAX_RULES 16
static Rule rules[MAX_RULES];
static uint8_t rule_count = 0;

bool addRule(const Rule& r) {
    if (rule_count >= MAX_RULES) return false; // reject, don't crash
    rules[rule_count++] = r;
    return true;
}
```

Preallocated limits are part of the system's contract, not arbitrary numbers:
- **Max 8 components** per node.
- **Max 16 local rules** per node.

If your change requires exceeding these limits — that's a signal you're solving the problem at the wrong architectural layer (you likely need the global rule engine on the Host, not a local one on the Node).

### 1.2 Static "vtable" for drivers, not runtime polymorphism with allocation

The driver registry (BME680, GPIO, etc.) is **static and known at compile time**. You add a new driver as an entry in a function table, not as a dynamically created object:

```cpp
typedef struct {
    const char* driver_id;
    bool (*init)(uint8_t addr);
    bool (*read)(uint8_t addr, SensorReading* out);
    uint32_t (*get_warmup_ms)(void);
} driver_vtable_t;

static const driver_vtable_t DRIVER_REGISTRY[] = {
    { "bme680", bme680_init, bme680_read, bme680_warmup_ms },
    { "gpio_digital", gpio_init, gpio_read, gpio_warmup_ms },
    // new driver -> new entry here
};
```

Virtual inheritance in C++ (`virtual`) is allowed where objects are created statically at startup (e.g. the transport layer, see `ARCHITECTURE.md` §2) — the problem is **dynamically creating/destroying objects at runtime**, not polymorphism itself.

### 1.3 The scheduler must respect sensor warm-up times

Every driver declares `warmup_ms` (the time between power-on/wake and a reliable reading). The scheduler **must not** poll a sensor before this time has elapsed — this isn't an optimization, it's data correctness (a BME680 will return garbage if polled too soon after cold start).

### 1.4 The frame format is a contract, not a suggestion

The 8-byte header (`ARCHITECTURE.md` §3.1) and frame types are a **stable contract** between Host/Bridge/Node. Changing the field layout, adding a new frame type, or changing flag semantics requires:
1. An entry in `packages/protocol` (the source of truth shared by TS and generated C headers).
2. Updating **all three** implementations (Host, Bridge, Node) in the same PR — a partial protocol migration in the field is a guaranteed way to get incompatibilities.
3. Version bumping in the Magic/Ver field, if the change isn't backward compatible.

---

## 2. Adding a new sensor/component — zero UI changes

This is the test of whether the manifest architecture actually works: **adding a new sensor type should not require changing a single line in `packages/web`.**

### Step by step

1. Add a manifest file at `packages/components/manifests/<id>.json` following the schema described in `ARCHITECTURE.md` §5 (`id`, `bus`, `params`, `outputs`, `power`).
2. If the component needs a firmware driver — add an entry to `DRIVER_REGISTRY` (see §1.2) in `firmware/node/drivers/`. That's the only place on the C++ side you touch.
3. **Don't edit** code in `packages/web` — the configuration form, the energy calculator, and the list of available `outputs` in the rule editor should generate automatically from the new manifest.
4. If manual testing shows the UI **didn't** generate correctly from the manifest alone — that's a bug in the form-rendering engine (`packages/web/src/manifest-form`), not a reason to write a custom React component for that one sensor.

### What to avoid

- ❌ `if (component.id === 'bme680') { return <Bme680Form />; }` — this is exactly the pattern the manifest architecture exists to eliminate.
- ❌ Hardcoding the string-key → integer-key mapping in more than one place. That mapping lives exclusively in `packages/protocol`.
- ❌ A manifest field that the UI has to specifically recognize by key name instead of by `type`. If you need a new form field type (e.g. `"type": "color"`), add generic support in the renderer, not in a specific manifest.

---

## 3. CBOR — key discipline

- All structures sent over radio use **integer keys**. Code review will reject any PR with a string key in a CBOR payload, no exceptions.
- The field-name ↔ integer-key mapping is centralized in `packages/protocol/src/field-map.ts` and the generated `firmware/common/protocol/field_map.h` header. These two files must be generated from a single source of truth (see `packages/protocol/README.md`) — manually editing both separately is forbidden, as it leads to Host/Node desync.
- Adding a new field to an existing structure (e.g. `rule_t`) requires considering MTU size — calculate the worst-case overhead (16 rules × record size) and make sure it fits within the fragmentation budget.

---

## 4. Testing and verification

- **Firmware**: changes to the CBOR parser, rule engine, and config store require unit tests running on-host (native, PlatformIO `test` environment) — don't rely solely on on-hardware testing.
- **Protocol**: every new frame type requires a fixture (a sample frame hex dump) in `packages/protocol/test/fixtures`, shared between TS and C++ tests.
- **Host/Web**: changes to the Graph→CBOR compiler require a round-trip test: React Flow graph → CBOR → deserialization → comparison against the expected rule structure.
- Changes touching the Duty Cycle Guard require an explicit test that the guard **blocks** a config exceeding the ETSI 1% limit — this is a regulatory safety feature, not just UX.

---

## 5. Commit and PR style

- One PR = one coherent layer change (don't mix a protocol change with a UI refactor).
- If a PR changes the frame format or config structure, describe in the PR **the protocol version before and after** and whether the change is backward compatible with nodes running an older version.
- Screenshots/GIFs are welcome for `packages/web` changes, especially in the graph editor and simulator.

---

Questions? Open a Discussion instead of an Issue if you don't yet have a concrete bug/feature request to file.
