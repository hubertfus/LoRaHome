# @lorahome/protocol

Source of truth for the frame format (8B header, frame types `0x01`-`0x41`) and the field-name ↔ CBOR integer-key mapping. Generates both TypeScript types (`src/field-map.ts`) and a C header (`firmware/common/protocol/field_map.h`) — never edit these two separately, see [CONTRIBUTING.md](../../CONTRIBUTING.md) §3.

Test fixtures (sample frame hex dumps) live in `test/fixtures`, shared between TS and C++ tests.
