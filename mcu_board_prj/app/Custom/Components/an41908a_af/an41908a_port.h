/**
 * @file    an41908a_port.h
 * @brief   AN41908A platform port for NE503 MCU
 * @note    Include this file and call an41908a_port_init() in your application
 */

#ifndef __AN41908A_PORT_H__
#define __AN41908A_PORT_H__

#include "an41908a.h"
#include "spi.h"

/**
 * @brief Get default configuration for NE503 board
 * @note  Pin mapping from main.h:
 *        - SPI1: PA4(CS), PA5(CLK), PA6(MISO), PA7(MOSI)
 *        - VD_IS: PD0, VD_FZ: PD1
 *        - PLS1: PD2, PLS2: PD3
 *        - RSTB: PD4
 *        - LENS_EN: PB3
 */
static inline void an41908a_get_default_cfg(an41908a_cfg_t *cfg)
{
    cfg->hspi = &hspi1;

    /* SPI CS - PA4 */
    cfg->cs_port = SPI1_CS_GPIO_Port;
    cfg->cs_pin = SPI1_CS_Pin;

    /* VD_IS - PD0 */
    cfg->vd_is_port = VD_IS_GPIO_Port;
    cfg->vd_is_pin = VD_IS_Pin;

    /* VD_FZ - PD1 */
    cfg->vd_fz_port = VD_FZ_GPIO_Port;
    cfg->vd_fz_pin = VD_FZ_Pin;

    /* RSTB - PD4 */
    cfg->rstb_port = RSTB_GPIO_Port;
    cfg->rstb_pin = RSTB_Pin;

    /* LENS_EN - PB3 */
    cfg->lens_en_port = LENS_EN_GPIO_Port;
    cfg->lens_en_pin = LENS_EN_Pin;

    /* PLS1 - PD2 (PI Zoom) */
    cfg->pls1_port = PLS1_GPIO_Port;
    cfg->pls1_pin = PLS1_Pin;

    /* PLS2 - PD3 (PI Focus) */
    cfg->pls2_port = PLS2_GPIO_Port;
    cfg->pls2_pin = PLS2_Pin;
}

/**
 * @brief Quick init with default NE503 configuration
 * @return 0 on success, -1 on failure
 */
static inline int an41908a_port_init(void)
{
    static an41908a_cfg_t cfg;
    an41908a_get_default_cfg(&cfg);
    return an41908a_init(&cfg);
}

#endif /* __AN41908A_PORT_H__ */
