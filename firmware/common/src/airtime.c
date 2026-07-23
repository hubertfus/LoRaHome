#include "lorahome/airtime.h"
#include <math.h>
#include <stdbool.h>

float lorahome_compute_airtime_ms(const lorahome_airtime_params_t* params) {
  const int sf = params->spreading_factor;
  const uint32_t bw = params->bandwidth_hz;
  const int cr = params->coding_rate;

  const bool low_data_rate_optimize = (sf >= 11 && bw <= 125000);
  const int de = low_data_rate_optimize ? 1 : 0;
  const int h = 0; /* explicit header always enabled */

  const float symbol_duration_ms = (float)((double)(1u << sf) / (double)bw) * 1000.0f;

  const float numerator = (float)(8 * params->bytes - 4 * sf + 28 + 16 - 20 * h);
  const float denominator = (float)(4 * (sf - 2 * de));
  float payload_symb_nb = 8.0f + fmaxf(ceilf(numerator / denominator) * (cr + 4), 0.0f);

  const float preamble_duration_ms = (params->preamble_symbols + 4.25f) * symbol_duration_ms;
  const float payload_duration_ms = payload_symb_nb * symbol_duration_ms;

  return preamble_duration_ms + payload_duration_ms;
}
