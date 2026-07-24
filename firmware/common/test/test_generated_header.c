/*
 * Compile-time verification of the generated protocol header.
 *
 * This translation unit exists to be compiled, on every target ABI we ship to,
 * with -Wall -Wextra -Werror. Nothing here runs on the host CI path: the entire
 * contract is checked by the compiler. If it builds clean on xtensa, riscv32
 * and x86-64, the header's layout claims hold on all three.
 *
 * It also pins the generated header against the hand-written protocol.h, which
 * still carries its own copies of the same constants. Two headers stating the
 * frame geometry is precisely the drift Etap 0 exists to make impossible, so
 * until protocol.h is folded into the generated one, the compiler enforces that
 * they agree.
 */
#include "lorahome/protocol.h"
#include "lorahome/protocol_generated.h"

/* ---- Generated header vs. hand-written protocol.h ------------------------ */
_Static_assert(LH_FRAME_MAGIC == LORAHOME_MAGIC_VER, "magic drift: generated vs protocol.h");
_Static_assert(LH_HEADER_SIZE == LORAHOME_HEADER_SIZE, "header size drift: generated vs protocol.h");
_Static_assert(LH_CRC_SIZE == LORAHOME_CRC_SIZE, "CRC size drift: generated vs protocol.h");
_Static_assert(LH_BROADCAST_ID == LORAHOME_BROADCAST_ID, "broadcast id drift: generated vs protocol.h");

_Static_assert((int)LH_TYPE_BEACON == (int)LORAHOME_FRAME_BEACON, "BEACON drift");
_Static_assert((int)LH_TYPE_JOIN_REQ == (int)LORAHOME_FRAME_JOIN_REQ, "JOIN_REQ drift");
_Static_assert((int)LH_TYPE_JOIN_ACK == (int)LORAHOME_FRAME_JOIN_ACK, "JOIN_ACK drift");
_Static_assert((int)LH_TYPE_CONFIG_BEGIN == (int)LORAHOME_FRAME_CONFIG_BEGIN, "CONFIG_BEGIN drift");
_Static_assert((int)LH_TYPE_CONFIG_FRAG == (int)LORAHOME_FRAME_CONFIG_FRAG, "CONFIG_FRAG drift");
_Static_assert((int)LH_TYPE_CONFIG_COMMIT == (int)LORAHOME_FRAME_CONFIG_COMMIT, "CONFIG_COMMIT drift");
_Static_assert((int)LH_TYPE_CONFIG_ACK == (int)LORAHOME_FRAME_CONFIG_ACK, "CONFIG_ACK drift");
_Static_assert((int)LH_TYPE_TELEMETRY == (int)LORAHOME_FRAME_TELEMETRY, "TELEMETRY drift");
_Static_assert((int)LH_TYPE_EVENT == (int)LORAHOME_FRAME_EVENT, "EVENT drift");
_Static_assert((int)LH_TYPE_CMD == (int)LORAHOME_FRAME_CMD, "CMD drift");
_Static_assert((int)LH_TYPE_CMD_ACK == (int)LORAHOME_FRAME_CMD_ACK, "CMD_ACK drift");
_Static_assert((int)LH_TYPE_CAPABILITY_REQ == (int)LORAHOME_FRAME_CAPABILITY_REQ, "CAPABILITY_REQ drift");
_Static_assert((int)LH_TYPE_CAPABILITY_RSP == (int)LORAHOME_FRAME_CAPABILITY_RSP, "CAPABILITY_RSP drift");

_Static_assert((int)LH_FLAG_ACK_REQ == (int)LORAHOME_FLAG_ACK_REQ, "ACK_REQ flag drift");
_Static_assert((int)LH_FLAG_FRAG == (int)LORAHOME_FLAG_FRAG, "FRAG flag drift");
_Static_assert((int)LH_FLAG_LAST == (int)LORAHOME_FLAG_LAST, "LAST flag drift");
_Static_assert((int)LH_FLAG_ENCR == (int)LORAHOME_FLAG_ENCR, "ENCR flag drift");

/* ---- Layout facts the roadmap calls out by name (R0.1) ------------------- */
_Static_assert(sizeof(lh_header_t) == 8, "header must be exactly 8 bytes on every ABI");
_Static_assert(offsetof(lh_header_t, seq) == 6, "seq offset drift");
_Static_assert(LH_MAX_PAYLOAD == 220, "MAX_PAYLOAD must be 220 at MTU 230");

/*
 * Exercises the generated accessors so they are type-checked rather than merely
 * parsed, and proves the byte order is a property of the bytes, not of the
 * host's endianness: src_id 0x0102 must occupy raw[2]=0x01, raw[3]=0x02 on
 * every target. Returns 0 on success so a host build can run it as a smoke test.
 */
int lh_generated_header_selftest(void);

int lh_generated_header_selftest(void) {
  lh_header_t hdr;
  uint8_t *raw = (uint8_t *)&hdr;

  for (unsigned i = 0; i < sizeof hdr; i++) {
    raw[i] = 0;
  }

  raw[LH_HDR_OFF_MAGIC_VER] = LH_FRAME_MAGIC;
  raw[LH_HDR_OFF_TYPE] = (uint8_t)LH_TYPE_TELEMETRY;
  lh_hdr_set_src_id(&hdr, 0x0102u);
  lh_hdr_set_dst_id(&hdr, LH_BROADCAST_ID);
  raw[LH_HDR_OFF_SEQ] = 0x2Au;
  raw[LH_HDR_OFF_FLAGS] = (uint8_t)LH_FLAG_ACK_REQ;

  if (raw[2] != 0x01u || raw[3] != 0x02u) return 1; /* big-endian on the wire */
  if (lh_hdr_get_src_id(&hdr) != 0x0102u) return 2;
  if (lh_hdr_get_dst_id(&hdr) != 0xFFFFu) return 3;
  if (raw[LH_HDR_OFF_SEQ] != 0x2Au) return 4;

  return 0;
}
