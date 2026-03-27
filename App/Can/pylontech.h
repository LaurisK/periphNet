/**
 * @file    pylontech.h
 * @brief   Pylontech CAN protocol definitions (500 kbps, 11-bit standard frames)
 *
 * This is the de-facto standard battery CAN protocol used by Pylontech, JK BMS,
 * and many other 48V LiFePO4 battery systems to communicate with inverters
 * (Solis, Deye, Victron, Growatt, SMA, etc.).
 *
 * All multi-byte values are little-endian.
 * All frames are 8 bytes unless noted otherwise.
 */

#ifndef PYLONTECH_H_
#define PYLONTECH_H_

#include <stdint.h>

/* --------------------------------------------------------------------------
 * CAN IDs
 * -------------------------------------------------------------------------- */

#define PYLON_CAN_ID_LIMITS     0x351   /* Charge/discharge voltage & current limits */
#define PYLON_CAN_ID_SOC        0x355   /* State of charge / health */
#define PYLON_CAN_ID_MEASURE    0x356   /* Voltage / current / temperature */
#define PYLON_CAN_ID_ALARM      0x359   /* Errors and warnings */
#define PYLON_CAN_ID_CHGCTRL    0x35C   /* Charge/discharge enable flags */
#define PYLON_CAN_ID_MFGNAME    0x35E   /* Manufacturer name (ASCII) */

#define PYLON_CAN_BAUDRATE      500000  /* 500 kbps */
#define PYLON_TX_INTERVAL_MS    1000    /* All frames sent every 1s */

/* --------------------------------------------------------------------------
 * Frame structures (packed, little-endian on ARM Cortex-M4)
 * -------------------------------------------------------------------------- */

/** 0x351 — Battery charge/discharge limits (DLC=8) */
typedef struct {
    int16_t  chargeVoltageLimit_dV;     /* 0.1 V resolution */
    int16_t  maxChargeCurrent_dA;       /* 0.1 A resolution */
    int16_t  maxDischargeCurrent_dA;    /* 0.1 A resolution */
    uint16_t dischargeVoltageLimit_dV;  /* 0.1 V resolution */
} __attribute__((packed)) sPylonLimits;

/** 0x355 — SOC / SOH (DLC=4) */
typedef struct {
    uint16_t soc_pct;       /* 0–100 % */
    uint16_t soh_pct;       /* 0–100 % */
} __attribute__((packed)) sPylonSoc;

/** 0x356 — Voltage / Current / Temperature (DLC=6) */
typedef struct {
    int16_t voltage_cV;     /* 0.01 V resolution */
    int16_t current_dA;     /* 0.1 A resolution (positive = discharge) */
    int16_t temperature_dC; /* 0.1 °C resolution */
} __attribute__((packed)) sPylonMeasure;

/** 0x359 — Errors and warnings (DLC=7) */
typedef struct {
    uint8_t errors0;        /* bit1=OV, bit2=UV, bit3=OT, bit4=UT, bit7=discharge OC */
    uint8_t errors1;        /* bit0=charge OC, bit3=system error */
    uint8_t warnings0;      /* bit1=high V, bit2=low V, bit3=high T, bit4=low T, bit7=discharge high I */
    uint8_t warnings1;      /* bit0=charge high I, bit3=internal error */
    uint8_t moduleNum;      /* module number (0x01) */
    uint8_t ascii_P;        /* 0x50 'P' */
    uint8_t ascii_N;        /* 0x4E 'N' */
} __attribute__((packed)) sPylonAlarm;

/** 0x35C — Charge/discharge enable (DLC=2) */
typedef struct {
    uint8_t flags;          /* bit7=charge EN, bit6=discharge EN, bit5=force charge I,
                               bit4=force charge II, bit3=full charge request */
    uint8_t reserved;
} __attribute__((packed)) sPylonChgCtrl;

/* Flag bits for sPylonChgCtrl.flags */
#define PYLON_FLAG_CHARGE_EN        0x80
#define PYLON_FLAG_DISCHARGE_EN     0x40
#define PYLON_FLAG_FORCE_CHARGE_I   0x20
#define PYLON_FLAG_FORCE_CHARGE_II  0x10
#define PYLON_FLAG_FULL_CHARGE_REQ  0x08

/** 0x35E — Manufacturer name (DLC=8, ASCII) */
#define PYLON_MFG_NAME  "PYLON   "

/* --------------------------------------------------------------------------
 * Aggregate: all battery data in one struct (for reader)
 * -------------------------------------------------------------------------- */

typedef struct {
    /* From 0x351 */
    float chargeVoltageLimit;       /* V */
    float maxChargeCurrent;         /* A */
    float maxDischargeCurrent;      /* A */
    float dischargeVoltageLimit;    /* V */

    /* From 0x355 */
    uint16_t soc;                   /* % */
    uint16_t soh;                   /* % */

    /* From 0x356 */
    float voltage;                  /* V */
    float current;                  /* A */
    float temperature;              /* °C */

    /* From 0x359 */
    uint8_t errors[2];
    uint8_t warnings[2];

    /* From 0x35C */
    uint8_t chargeEnabled;
    uint8_t dischargeEnabled;

    /* From 0x35E */
    char manufacturer[9];           /* null-terminated */

    /* Receive tracking */
    uint32_t rxMask;                /* bit per CAN ID received (for completeness check) */
    uint32_t lastRxTick;            /* HAL_GetTick() of last frame received */
} sPylonBatteryData;

#define PYLON_RX_GOT_LIMITS   (1U << 0)
#define PYLON_RX_GOT_SOC      (1U << 1)
#define PYLON_RX_GOT_MEASURE  (1U << 2)
#define PYLON_RX_GOT_ALARM    (1U << 3)
#define PYLON_RX_GOT_CHGCTRL  (1U << 4)
#define PYLON_RX_GOT_MFGNAME  (1U << 5)
#define PYLON_RX_GOT_ALL      0x3F

#endif /* PYLONTECH_H_ */
