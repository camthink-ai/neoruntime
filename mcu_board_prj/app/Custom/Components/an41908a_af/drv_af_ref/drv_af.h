#ifndef __DRV_AF_H__
#define __DRV_AF_H__

#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <asm/unistd.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

#include <linux/bitrev.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>

#include "ms_af_common.h"
#include "gpio_3516dv300.h"


// use for debug information
//#define _AF_DEBUG
#ifdef _AF_DEBUG
#define DRV_AF_DBG(fmt, args...) printk("AF Debug: " fmt, ## args)
#else
#define DRV_AF_DBG(fmt, args...)
#endif

#define _ABF_DEBUG
#ifdef _ABF_DEBUG
#define DRV_ABF_DBG(fmt, args...) printk("ABF Debug: " fmt, ## args)
#else
#define DRV_ABF_DBG(fmt, args...)
#endif

#define DRV_AF_ERR(fmt, args...) printk(KERN_ERR "AF Error: " fmt, ## args)







/***********************************************
 *
 * 	Define motor base information
 *	Version 1.0.0	: add for drv_af modules
 *
 ***********************************************/ 
/* Base define */
#define AF_MOUDLE_VERSION	"1.0.0"
#define DEVICE_AF_NAME 		"drv_af"		//device name


#define VD_DELAY			udelay(5)

typedef enum 
{
	AF_MODE_NONE = -1,
	AF_MODE_ZOOM = 0,
	AF_MODE_FOCUS,
	AF_MODE_BOTH,

}af_mode_e;

typedef struct a5s_af_idx_s
{
	VU32	zm_pps;
	Vint32 	zm_idx;
	VU32	fs_pps;
	Vint32  fs_idx;

}af_idx_t;

typedef struct drv_af_s
{
	// Use for dist control
	VU32	dist_zm;
	VU32	dist_fs;

	Vint32	zm_idx;
	Vint32	fs_idx;

	VU16	zm_steps;
	VU16	fs_steps;

	VU32	vd_PSUM_zm;
	VU32	vd_PSUM_fs;

	Vint32	vd_num_zm;
	Vint32	vd_num_fs;
	
	// Use for af check busy
	Vint8	zm_busy;
	Vint8	fs_busy;

	VU8		zm_irq_num;
	VU8		fs_irq_num;

	Vint8	zm_irq_dn;
	Vint8	fs_irq_dn;

	af_mode_e af_mode;

	// DC-iris duty
	VU16	iris_duty;
	VU16    af_exit;
	
	// Use for af lens step control. 
	volatile af_idx_t	af_cfg;
	Vint32	lens_optical_gpio;
	
}drv_af_t;

typedef struct gpio_irq_info {
	int flag;
	int irq_gpio;
	int	irq_num;
	int	irq_type;
	
}gpio_irq_info_t;

typedef struct af_irq_s {	
	gpio_irq_info_t *irq_info_zm;
	gpio_irq_info_t *irq_info_fs;
	drv_af_t        *af_info;

}af_iq_t;

typedef struct abf_irq_s {	
	gpio_irq_info_t *irq_info_abf;
	drv_af_t        *af_info;

}abf_iq_t;


// Error number
typedef enum tagAF_ERROR_E
{
	AF_STATUS_EFAIL = -1,
	AF_STATUS_SOK

}AF_ERROR_E;


#endif /* __DRV_AF_H__ */

