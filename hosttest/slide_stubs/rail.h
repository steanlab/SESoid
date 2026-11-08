#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef void *RAIL_Handle_t;
typedef uint64_t RAIL_Events_t;
typedef int16_t RAIL_TxPower_t;
typedef int RAIL_Status_t;
uint32_t RAIL_GetTime(void);
RAIL_Status_t RAIL_SetTxPowerDbm(RAIL_Handle_t h, RAIL_TxPower_t p);
RAIL_TxPower_t RAIL_GetTxPowerDbm(RAIL_Handle_t h);
