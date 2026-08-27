/*
 * pack_types.h
 *
 * The registration roster: every pack type this firmware carries, declared in
 * one place so the composition root does not forward-declare them by hand.
 *
 * A TYPE FILE IS OTHERWISE PRIVATE.  This header exposes exactly one symbol
 * per type -- the registration call -- and nothing about the protocol behind
 * it.  Adding a type means adding a line here and a line in the composition
 * root; nothing else outside the type learns it exists.
 *
 * NOT a consumer header: a consumer includes App/Pack/pack.h and nothing else
 * (docs/design_battery_pack.md §9).
 */

#ifndef PACK_TYPES_H_
#define PACK_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register the JK BMS type (pull, Modbus RTU).
 * @retval packErr_ok, or packErr_full / packErr_busy
 * @note   Call BEFORE Func_Start.  A type must be registered before
 *         Pack_Init binds anything, or its instances come up
 *         packWhy_noType.
 */
int PackJkBms_Register(void);

/**
 * @brief  Register the Pylontech type (push, CAN).
 *
 * REGISTERS BUT CANNOT BIND: App/Can has no RX dispatcher yet, so every
 * instance comes up absent with packWhy_typeUnavailable -- the operator's
 * configuration is correct and the firmware is the limitation.  Registering
 * it anyway is what makes that distinction reportable instead of silent.
 *
 * @retval packErr_ok, or packErr_full / packErr_busy
 */
int PackPylontech_Register(void);

#ifdef __cplusplus
}
#endif

#endif /* PACK_TYPES_H_ */
