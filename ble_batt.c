// SES V300 (not badge154): battery monitoring, see ble_batt.h
#include "ble_batt.h"

static uint16_t th_low = 2700, th_crit = 2500;
static uint16_t last_mv;
static uint8_t level;
static uint32_t last_s;
static bool have;
static uint32_t last_try_s;                      // last failed attempt: do not hammer the converter

#ifdef BATT_HOST
uint16_t batt_host_mv;                                  // host test: the "measured" voltage
static uint16_t hw_read_mv(void) { return batt_host_mv; }
#else
#include "em_cmu.h"
#include "em_iadc.h"
#include "ble_sys.h"
// one conversion of AVDD/4 against the internal 1.21 V reference, 12 bit: AVDD[mV] = code * 1210 * 4 / 4095
static uint16_t hw_read_mv(void)
{
  CMU_ClockEnable(cmuClock_IADC0, true);
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t cfg = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t single = IADC_INITSINGLE_DEFAULT;
  IADC_SingleInput_t input = IADC_SINGLEINPUT_DEFAULT;
  init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 20000000u, 0);
  cfg.configs[0].reference = iadcCfgReferenceInt1V2;
  cfg.configs[0].analogGain = iadcCfgAnalogGain1x;
  cfg.configs[0].adcClkPrescale = IADC_calcAdcClkPrescale(IADC0, 1000000u, 0, iadcCfgModeNormal, init.srcClkPrescale);
  single.dataValidLevel = iadcFifoCfgDvl1;                    // one conversion: the FIFO flag must rise after one result (the default waits for four)
  input.posInput = iadcPosInputAvdd;
  input.negInput = iadcNegInputGnd;
  IADC_init(IADC0, &init, &cfg);
  IADC_initSingle(IADC0, &single, &input);
  uint32_t sum = 0, n = 0;
  for (int k = 0; k < 8; k++) {
    IADC_command(IADC0, iadcCmdStartSingle);
    uint32_t guard = 0;
    while (!(IADC_getStatus(IADC0) & IADC_STATUS_SINGLEFIFODV) && ++guard < 200000u) { }
    if (guard >= 200000u) break;
    sum += IADC_pullSingleFifoResult(IADC0).data & 0xFFFu;
    n++;
  }
  IADC_reset(IADC0);
  CMU_ClockEnable(cmuClock_IADC0, false);
  if (!n) return 0;
  return (uint16_t)((sum * 1210u * 4u) / (n * 4095u));
}
#endif

void batt_set_thresholds(uint16_t low_mv, uint16_t crit_mv)
{
  th_low = low_mv; th_crit = crit_mv < low_mv ? crit_mv : low_mv;
}

static void classify(uint16_t mv)
{
  uint8_t r = mv <= th_crit ? BATT_CRIT : (mv <= th_low ? BATT_LOW : BATT_OK);
  if (r >= level) { level = r; return; }                        // worse (or equal): at once
  uint16_t leave = level == BATT_CRIT ? th_crit : th_low;       // better: only when clearly above the threshold of the level being left
  if (mv > leave + BATT_HYST_MV) level = r;
}

void batt_measure_now(uint32_t now_s)
{
  uint16_t mv = hw_read_mv();
  if (!mv) { last_try_s = now_s; return; }
  last_mv = mv; last_s = now_s; have = true;
  classify(mv);
}

void batt_poll(uint32_t now_s, bool panel_idle)
{
  if (!panel_idle) return;
  if (!have) { if (now_s >= BATT_FIRST_S && (last_try_s == 0 || (uint32_t)(now_s - last_try_s) >= 10u)) batt_measure_now(now_s); return; }
  if ((uint32_t)(now_s - last_s) >= BATT_PERIOD_S) batt_measure_now(now_s);
}

uint8_t batt_level(void) { return level; }
uint16_t batt_mv(void) { return have ? last_mv : 0u; }
uint32_t batt_age_s(uint32_t now_s) { return have ? now_s - last_s : 0xFFFFFFFFu; }
