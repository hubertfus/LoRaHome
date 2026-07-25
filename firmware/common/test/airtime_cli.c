/*
 * Host-native airtime harness for cross-language verification (T1.3).
 *
 * Reads `<bytes> <sf> <bandwidth_hz> <coding_rate> <preamble>` per line on
 * stdin and writes the computed time-on-air in milliseconds, six decimals, one
 * per line.
 *
 * Deliberately dumb, for the same reason crc16_cli.c is: the TypeScript side
 * owns the parameter grid and pipes it in, so the only thing under test here is
 * lorahome_compute_airtime_ms(). If the two languages disagree, the harness is
 * not a plausible suspect.
 *
 * Why this matters more than most cross-checks: this number is what the duty
 * cycle tracker spends. If the Host and the Bridge disagree about what a frame
 * costs on air, one of them is wrong about ETSI EN 300 220 compliance, and that
 * is the one bug in this system with a legal consequence rather than a
 * technical one.
 */
#include <stdio.h>

#include "lorahome/airtime.h"

int main(void) {
  unsigned bytes, sf, bandwidth, coding_rate, preamble;

  while (scanf("%u %u %u %u %u", &bytes, &sf, &bandwidth, &coding_rate, &preamble) == 5) {
    const lorahome_airtime_params_t params = {
        (uint16_t)bytes,
        (uint8_t)sf,
        (uint32_t)bandwidth,
        (uint8_t)coding_rate,
        (uint8_t)preamble,
    };
    printf("%.6f\n", (double)lorahome_compute_airtime_ms(&params));
  }

  return 0;
}
