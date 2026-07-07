/**
 * @file    modbus_default_config.h
 * @brief   Built-in default Modbus config (Solis inverter) — compiled
 *          through the same JSON pipeline as any uploaded config, so no
 *          special-cased register code survives (design §13).
 */
#ifndef MODBUS_DEFAULT_CONFIG_H_
#define MODBUS_DEFAULT_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* The factory Solis config as JSON (also a user-facing reference for the
 * authoring format — download it via /api/modbus/config/download). */
extern const char g_modbusDefaultConfigJson[];

/* If no valid active config region exists, compile the built-in default
 * into it. Idempotent; called from HTTP server init (boot) and walker
 * start. 0 = a valid active config exists (either way), -1 = failure. */
int ModbusConfig_EnsureDefault(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_DEFAULT_CONFIG_H_ */
