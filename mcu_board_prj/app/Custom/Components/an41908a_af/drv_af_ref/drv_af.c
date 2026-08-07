/****
 * 
 * drv_af.c
 * 
 * Des:
 *   AF lens control driver.
 * 
 * History:
 *  2015/05/25  - [eric@milesight.cn] Create file.
 *  2016/07/11  - [wink@milesight.cn] modify file.
 * 
 * Copyright (C) 2011-2015, Milesight, Inc.
 * 
 * All rights reserved. No Part of this file may be reproduced, stored
 * in a retrieval system, or transmitted, in any form, or by any means,
 * electronic, mechanical, photocopying, recording, or otherwise,
 * without the prior consent of Milesight, Inc.
 * 
 */

#include "drv_af.h"
#include "af_an41908a.h"
#include "af_bu24036mwv.h"
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/gpio.h>

#include <linux/uaccess.h>




#ifndef AF_ABS
#define AF_ABS(a)	   (((a) < 0) ? -(a) : (a))
#endif

static void  *reg_iocfg_base  = 0;
#define AF_WRITEL(Addr, Value) ((*(volatile unsigned int *)(Addr)) = (Value))

#define GPIO_BASE_IRQ			(29)  // IRQ number

#define GPIO_SPI2_CLK			MS_GPIO(3, 4)
#define GPIO_SPI2_SDO			MS_GPIO(3, 5)
#define GPIO_SP2_SDI			MS_GPIO(3, 6)
#define GPIO_SPI2_CS0			MS_GPIO(3, 7)
typedef struct
{
	VU32	gio_id;			// GPIO id number.
	VU32	gio_mux;		// GPIO mux REG number.
	VU32	gio_mux_data;	// GPIO mux REG configure data.
} SPI_GPIO_CFG_T;
static SPI_GPIO_CFG_T g_spi_gpio[] = {
	{GPIO_SPI2_CLK, 75, 0},
	{GPIO_SPI2_SDO, 76, 0},
	{GPIO_SP2_SDI, 77, 0},
	{GPIO_SPI2_CS0, 78, 0}, 
};
#define GPIO_CFG_SIZE	(sizeof(g_spi_gpio)/sizeof(g_spi_gpio[0]))

static DEFINE_SEMAPHORE(af_sem);
static DEFINE_SPINLOCK(gpio_spi_spinlock);
static DEFINE_SPINLOCK(af_spinlock);
//#define AF_DEBUG 1
/* hr timer control */
static struct hrtimer af_hr_timer;
static ktime_t af_ktime;
static struct completion   af_timer_complete;
/* Define zoom focus check irq */
static VU32	g_register_irq = 0;
static gpio_irq_info_t gpio_ext_zm;
static gpio_irq_info_t gpio_ext_fs;
static gpio_irq_info_t gpio_ext_abf;
static af_iq_t*	g_irq_info = NULL;
static volatile af_init_params_t g_af_info =
{
	.chip_type = CHIP_AN41908,
	.zm_max_pps = 800, 
	.fs_max_pps = 800,
//	.ircut_max_pps = 800,
	.iris_open_bd = 1023,
	.spi_mode = SPI_GPIO,
	.zpi_status = PI_EN,
	.fpi_status = PI_EN,
};
static volatile lens_drv_gpio_t g_stGpio;
static drv_af_t g_af_ctl;
static volatile af_dist_t	g_af_dist;
static VU8	g_has_cache = 0;
static volatile af_dist_t	g_af_dist_cache;

/* Define default af lens arguments */
static volatile u32 g_vd_PSUMxx_zm;
static volatile u32 g_vd_INTCTxx_zm;
static volatile u32 g_vd_PSUMxx_fs;
static volatile u32 g_vd_INTCTxx_fs;

static int af_spi_init(void);
static inline void af_usleep(u32 usec);
static inline void af_msleep(u32 msec);


//===================================================//
// Define AN41908a driver API
//===================================================//
/*add by Goli*/
static u16 an41908a_spi_read8( u8 addr, u16 *output);
static int an41908a_spi_write8(u8 addr, u16 wt_data);
/*end by Goli*/

static int an41908a_cmd_write(u8 addr, u16 wt_data);
static u16 an41908a_cmd_read(u8 addr, u16* outdata);
static inline void an41908a_wait_zm_stop(void);
static inline void an41908a_wait_fs_stop(void);
static int an41908a_init_reg(void);
static inline int an41908a_check_busy(void);
static int an41908a_out_vd(const u32 vd_num);
static int an41908a_set_aperture(u16 aperture_idx);
static int an41908a_set_ircut(u8 enable);
static int an41908a_set_zm_pi(u16 z_pi,u16 speed);
static int an41908a_set_fs_pi(u16 f_pi,u16 speed);
static int an41908a_set_zf_pi(u16 z_pi, u16 f_pi,u16 speed);
static int an41908a_zoom_run(u16 pps, int distance);
static int an41908a_focus_run(u16 pps, int distance);
static int an41908a_zoom_stop(void);
static int an41908a_focus_stop(void);
static int an41908a_zf_sync(u16 zm_pps, s32 zm_dist, u16 fs_pps, s32 fs_dist);
//static int an41908a_cache_run(void);
static int an41908a_run_cmd(int cmd, void* args);

static irqreturn_t an41908a_busy_irq(int irq, void *dev_id);
static inline int an41908a_CheckState(gpio_irq_info_t *irq_cfg);
static inline void an41908a_ClrIntState(gpio_irq_info_t *irq_cfg);
static inline int an41908a_initIntState(gpio_irq_info_t *irq_cfg);
static inline int an41908a_init_irq(void);
static inline void an41908a_irq_free(void);
static inline int abf_init_irq(void);


//===================================================//
// Define BU24036mwv driver API
//===================================================//
static int bu24036mwv_spi_write8(u16 wt_data);
static int bu24036mwv_write(u16 wdata);
static inline void bu24036mwv_wait_zm_stop(void);
static inline void bu24036mwv_wait_fs_stop(void);
static int bu24036mwv_zoom_stop(void);
static int bu24036mwv_focus_stop(void);
static int bu24036mwv_zf_init(driver_lens_type_e driver_type);
static int bu24036mwv_iris_init( void );
static int bu24036mwv_init_reg(driver_lens_type_e driver_type);
static inline int bu24036mwv_check_busy(void);
static inline int bu24036mwv_check_run(void);
static int bu24036mwv_zf_run(u16 zm_pps, s32 zm_dist, u16 fs_pps, s32 fs_dist);
static int bu24036mwv_run_cmd(int cmd, void* args);

extern int set_ircut_pi(void);
extern int ircut_origin_set(void);
extern int register_ircut_cb(void);
extern int run_twoLines_ircut(int pos);
extern int run_fourLines_ircut(int pos);


static int double_run_cmd(int cmd, void* args);


//drv_8833����
volatile Int32 drv8833_irq_number = 0;  // ������Ҫ�ߵĲ�������Ӧ���жϴ���
volatile Int32 drv8833_step_number = 0; // ������Ҫ�ߵĲ���
volatile Int32 interval    = 0;	        // 1us
volatile Int32 drv8833_dir = 1;		    // 1��,-1��
volatile Int32 zm2_busy     = 0;         // focus״̬
volatile Int32 drv8833_up  = 1;         // 8833��������־
volatile Int32 an41908_up  = 1;         // 41908��������־
volatile Int32 fs_idx      = 0;         // focus����PI��λ��
volatile Int32 zm1_idx   = 0; 
volatile Int32 zm2_idx   = 0; 
volatile Int32 switch_case = 0;
volatile Int32 zm2_pitime =0;

struct  hrtimer htimer;                 // �߷ֱ��ʶ�ʱ���ṹ��

//8833��������
static int htimer_init(void);
static int drv_8833_gpioinit(void);
static int drv_8833_step_run(void);  
static int drv8833_run(u16 pps, int distance);
static int drv8833_set_pi(void);
static inline int drv8833_check_busy(void);

//..................................�߷ֱ��ʶ�ʱ��................................................................

//��ʱ����������
static enum hrtimer_restart myhtime(struct hrtimer *timer)
{
	unsigned long flags = 0;
	if((drv8833_irq_number--)>0)
	{
	    drv_8833_step_run();
	    hrtimer_forward_now(&htimer,ktime_set(interval/1000000,interval%1000000*1000)); 
		return HRTIMER_RESTART;
	}
	else
	{  
		spin_lock_irqsave(&af_spinlock, flags);
	    zm2_busy=0;
	    drv8833_up = 1;
	    if(drv8833_up&&an41908_up)
	    {   
			up(&af_sem);
	    }
		spin_unlock_irqrestore(&af_spinlock, flags);
		return HRTIMER_NORESTART;
	}
}

static int htimer_init(void)  
{
	hrtimer_init(&htimer,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
	htimer.function = myhtime;
	hrtimer_start(&htimer,ktime_set(interval/1000000,interval%1000000*1000),HRTIMER_MODE_REL);  //�򿪶�ʱ��
	return 0;
}   

//....................................drv8833GPIO..............................................................

static int drv_8833_gpioinit(void)    //����ģ����ʱ����Ҫ
{
	hisi_gpio_mode(g_stGpio.GIO_8833_A1,GIO_OUTPUT);
	hisi_gpio_mode(g_stGpio.GIO_8833_A2,GIO_OUTPUT);
	hisi_gpio_mode(g_stGpio.GIO_8833_B1,GIO_OUTPUT);
	hisi_gpio_mode(g_stGpio.GIO_8833_B2,GIO_OUTPUT);
	hisi_gpio_mode(g_stGpio.GIO_8833_PI_FS,GIO_NOIRQ_INPUT);
	return 0;
}

//.....................................drv_8833_step_run.......................................................

static int drv_8833_step_run(void)   //ÿ����1�ε�����һ����ǡ�
{   
	zm2_idx +=drv8833_dir;
	switch_case = (1600+zm2_idx)%8;
	switch(switch_case)   
	{
		case 7:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 0);
			break;	
		case 6:
		 	hisi_gpio_write(g_stGpio.GIO_8833_A1, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 1);
			break;
		case 5:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 1);
			break;
		case 4:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 1);
			break;
		case 3:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 0);
			break;
		case 2:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 0);
			break;
		case 1:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 0);
			break;
		case 0:
			hisi_gpio_write(g_stGpio.GIO_8833_A1, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_A2, 0);
			hisi_gpio_write(g_stGpio.GIO_8833_B1, 1);
			hisi_gpio_write(g_stGpio.GIO_8833_B2, 0);
			break;
		default:
			break;
	}
	return 0;
}

//....................................drv_8833_step_run.......................................................

static int drv8833_run(u16 pps, int distance)
{  
   //�Ը��������ʼ��
	drv8833_dir = 0;
	drv8833_step_number = 0;
	drv8833_irq_number = 0;
    drv8833_up = 0;
	
	if(pps==0||distance==0)   //�ٶȻ��߾���Ϊ0ʱ�˳�
	{
		return 1;
	}
	
	zm2_busy=1;
	
	drv8833_dir = (distance>0) ? 1 : -1;  // Direction.
	distance    = (distance>0) ? distance : (-distance);  // Absolute distance.

	drv8833_step_number =  distance;             // Step count.
	drv8833_irq_number  =  drv8833_step_number;  // Interrupt count.
	interval = 1000000/pps;                      // Interrupt interval in us.

	htimer_init();   // Initialize and start the timer.
	return 0;
}

//....................................focus PI.......................................................
static int drv8833_set_pi()
{
	int zm2_pi = 0;
	
	zm2_busy=1;
	hisi_gpio_mode(g_stGpio.GIO_8833_PI_FS,GIO_NOIRQ_INPUT);
	hisi_gpio_write(g_stGpio.GIO_LENS_PI_POWER_EN, LENS_LIGHT_ON); //open lens optocoupler
	af_msleep(10);
	zm2_pi=hisi_gpio_read(g_stGpio.GIO_8833_PI_FS);
	zm2_pitime = 0;
	if (zm2_pi == 0) {
		if (g_af_info.z2pi_status == PI_DN) {
			drv8833_dir = -1;
		} else {
			drv8833_dir = 1;
		}
//		drv8833_dir = -1;
		while (zm2_pi == 0) {
			zm2_pitime++;
			drv_8833_step_run();
			af_msleep(10);
			zm2_pi = hisi_gpio_read(g_stGpio.GIO_8833_PI_FS);
			if (zm2_pitime >= 60000) {     // 600s timeout
				break;
			}
		}
		printk("zm2_pitime=%d\n", zm2_pitime);
	}
	zm2_pitime = 0;
	zm2_pi = hisi_gpio_read(g_stGpio.GIO_8833_PI_FS);
	if (g_af_info.z2pi_status == PI_DN) {
		drv8833_dir = 1;
	} else {
		drv8833_dir = -1;
	}
//	drv8833_dir = 1;
	while (zm2_pi == 1) {
		zm2_pitime++;
		drv_8833_step_run();
		af_msleep(10);
		zm2_pi = hisi_gpio_read(g_stGpio.GIO_8833_PI_FS);
		if (zm2_pitime >= 60000) {     // 600s timeout
			break;
		}
	}
	printk("zm2_pitime=%d\n", zm2_pitime);

#if 0
	if(zm2_pi==0)
	{   
		drv8833_dir = -1;
		while(zm2_pi==0)
		{   
		    zm2_pitime++;
		    drv_8833_step_run(); 
			af_msleep(10);
			zm2_pi=hisi_gpio_read(g_stGpio.GIO_8833_PI_FS);	
		}
		printk("zm2_pitime=%d\n", zm2_pitime);
	}
	else
	{
		drv8833_dir = 1;
		while(zm2_pi==1)
		{   
	     	zm2_pitime++;
		    drv_8833_step_run(); 
			af_msleep(10);
			zm2_pi=hisi_gpio_read(g_stGpio.GIO_8833_PI_FS);
		}
		printk("zm2_pitime=%d\n", zm2_pitime);		
	}
#endif
	hisi_gpio_write(g_stGpio.GIO_LENS_PI_POWER_EN, LENS_LIGHT_OFF); //open lens optocoupler
	if (zm2_pitime >= 60000) {    // 600s timeout
		DRV_AF_ERR("Zoom2 PI failed\n");
		return 0;
	}
	if(switch_case%2==0) // When switch_case is even, advance focus by one step.
	{
	   drv_8833_step_run(); 
	   zm2_idx = zm2_idx%8;
	}
	zm2_busy = 0;

    return 0;
}

//....................................drv8833_check_busy.......................................................

static inline int drv8833_check_busy(void)
{
	int ret;
	unsigned long flags = 0;
	
	spin_lock_irqsave(&af_spinlock, flags);
	ret = zm2_busy;
	spin_unlock_irqrestore(&af_spinlock, flags);
	return ret;
}
//......................................spi_read....................................................

static u16 an41908a_spi_read8( u8 addr, u16 *output)
{
	u16 tmp_rddata1= 0,tmp_rddata2 = 0;
	u8 i;
	unsigned long flags = 0;
	
	spin_lock_irqsave(&gpio_spi_spinlock, flags);
	hisi_gpio_write(GPIO_SPI2_CS0, 0);	//set stop bit
	asm("nop");
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CLK, 1);
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CS0, 1);	//set start bit
	for ( i = 0; i <8; i++)
	{
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 0);
		asm("nop");
		asm("nop");
		if( i<= 5)
		{
			hisi_gpio_write(GPIO_SPI2_SDO, ((addr&(1<<i)) >> i));  //set write bit
		} 
		else
	 	{
			hisi_gpio_write(GPIO_SPI2_SDO, 1);  //set read bit
		}
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 1);
		asm("nop");
		asm("nop");
		asm("nop");
		asm("nop");
	}

	asm("nop");
	asm("nop");

	/*read data*/
	for ( i = 0; i < 16; i++)
	{
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 0);
		asm("nop");
		asm("nop");
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 1);
		asm("nop");
		asm("nop");
		tmp_rddata1 = hisi_gpio_read(GPIO_SP2_SDI);
		tmp_rddata1 = tmp_rddata1 << i;
		tmp_rddata2 |= tmp_rddata1;
		asm("nop");
		asm("nop");
	}
	
	*output= tmp_rddata2;
	asm("nop");
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CS0, 0);	//set stop bit
	asm("nop");
	asm("nop");

	spin_unlock_irqrestore(&gpio_spi_spinlock, flags);
	return 0;
}


//......................................spi_write....................................................

static int an41908a_spi_write8(u8 addr, u16 wt_data)
{
	u8 i; 
	unsigned long flags = 0;
	
	spin_lock_irqsave(&gpio_spi_spinlock, flags);
	hisi_gpio_write(GPIO_SPI2_CS0, 0);	//set stop bit
	asm("nop");
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CLK, 1);
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CS0, 1);	//set start bit	
	for ( i = 0; i < 8; i++)
	{
		asm("nop");
  		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 0);
		asm("nop");
		asm("nop");
		if( i<= 5)
		{ 
			hisi_gpio_write(GPIO_SPI2_SDO, ((addr&(1<<i)) >> i));
		} 
		else
	 	{
			hisi_gpio_write(GPIO_SPI2_SDO, 0);  //set write bit
		}
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 1);
		asm("nop");
		asm("nop");
		asm("nop");
		asm("nop");
	}

	/*write input data*/
	for ( i = 0; i < 16; i++)
	{
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 0);
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_SDO, ((wt_data&(1<<i)) >> i));
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 1);
		asm("nop");
		asm("nop");
		asm("nop");
		asm("nop");
	}
	
	asm("nop");
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CS0, 0);	//set stop bit
	asm("nop");
	asm("nop");
	spin_unlock_irqrestore(&gpio_spi_spinlock, flags);
	
	return 0;
}

static int bu24036mwv_spi_write8(u16 wt_data)
{
	s8 i;
	unsigned long flags = 0;
	
	spin_lock_irqsave(&gpio_spi_spinlock, flags);
	hisi_gpio_write(GPIO_SPI2_CS0, 1);	//set stop bit
	asm("nop");
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CLK, 1);
	asm("nop");
	hisi_gpio_write(GPIO_SPI2_CS0, 0);	//set start bit
  	asm("nop");
  	asm("nop");
	for ( i = 15; i >= 0; i--)
	{
		/*write input data*/
		hisi_gpio_write(GPIO_SPI2_CLK, 0);
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_SDO, ((wt_data&(1<<i)) >> i));
		asm("nop");
		asm("nop");
		hisi_gpio_write(GPIO_SPI2_CLK, 1);
		asm("nop");
		asm("nop");
		asm("nop");
		asm("nop");
	}
	hisi_gpio_write(GPIO_SPI2_CS0, 1);	//set stop bit
	asm("nop");
	asm("nop");
	spin_unlock_irqrestore(&gpio_spi_spinlock, flags);

	return 0;
}

/* hr timer control begin */
static void  af_hrtimer_cancel( void )
{
	int ret;

	ret = hrtimer_cancel( &af_hr_timer );   //init hr_timer
	if (ret){
		printk( KERN_ALERT "The udx timer was still in use...\n");
	} else {
		printk( KERN_ALERT "udx timer cancel! \n");
	}
}

static enum hrtimer_restart af_hrtimer_callback( struct hrtimer *timer)
{
	complete(&af_timer_complete);
	return HRTIMER_NORESTART;
}

#define US_TO_NS(x)	((u32)(((u32)x) * (u32)1000L))
#define MS_TO_NS(x)	((u32)(((u32)x) * (u32)1000000L))
static void af_wait_timeout(u32 usec)  //usec == microsecond
{
	af_ktime = ktime_set( 0, US_TO_NS(usec));                  
	hrtimer_init( &af_hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	af_hr_timer.function = af_hrtimer_callback;
	hrtimer_start( &af_hr_timer, af_ktime, HRTIMER_MODE_REL );
}

/* hr timer control end */

static void af_msleep(u32 msec)
{
	init_completion(&af_timer_complete);
	af_wait_timeout(msec*1000);
	wait_for_completion(&af_timer_complete);
}

static void af_usleep(u32 usec)
{
	init_completion(&af_timer_complete);
	af_wait_timeout(usec);
	wait_for_completion(&af_timer_complete);
}

static inline void an41908a_wait_zm_stop(void)
{
	int max_msec = (VD_MDELAY_DEF+5);
	// wait for front opration finish.
	do {
		af_usleep(500);
		max_msec--;
		if(!max_msec)
		{
			DRV_AF_ERR("Failed check zm irq \n");
			break;
		}
	}while(!g_af_ctl.zm_irq_dn);
}

static inline void an41908a_wait_fs_stop(void)
{
	int max_msec = (VD_MDELAY_DEF+5);
	// wait for front opration finish.
	do {
		af_usleep(500);
		max_msec--;
		if(!max_msec)
		{
			DRV_AF_ERR("Failed check fs irq \n");
			break;
		}
	}while(!g_af_ctl.fs_irq_dn);
}

static inline void bu24036mwv_wait_zm_stop(void)
{
	int bbusy;
	do {
		af_usleep(500);
		bbusy = hisi_gpio_read(g_stGpio.GIO_BU24_B1);  //Goli.milesight modify
		bbusy |= hisi_gpio_read(g_stGpio.GIO_BU24_B2); //Goli.milesight modify
	}while(bbusy);
}

static inline void bu24036mwv_wait_fs_stop(void)
{
	int bbusy;
	do {
		af_usleep(500);
		bbusy = hisi_gpio_read(g_stGpio.GIO_BU24_A1);  //Goli.milesight modify
		bbusy |= hisi_gpio_read(g_stGpio.GIO_BU24_A2);//Goli.milesight modify
	}while(bbusy);
}


/***********************************************************
*
*	For SPI control.
*
***********************************************************/
#define SPI_MSG_NUM		20
typedef struct hi_spi_message_s
{
	struct spi_transfer	t;
	struct spi_message	m;
	unsigned char	buf[8];

} spi_message_s;

typedef struct hi_spi_message_info_s
{
	int msg_idx;
	spi_message_s spi_msg_array[SPI_MSG_NUM];

} spi_message_info_s;

static spi_message_info_s g_spi_msg = {0};
static struct spi_master* af_spi_master;
static struct spi_device* af_spi;
extern struct bus_type   spi_bus_type;

static int af_spi_bus = 2;
module_param(af_spi_bus, uint, S_IRUGO);
MODULE_PARM_DESC(af_spi_bus, "SPI controller ID");
static int af_spi_cs = 0;
module_param(af_spi_cs, uint, S_IRUGO);
MODULE_PARM_DESC(af_spi_cs, "SPI CS");

static int af_spi_init(void)
{
	int 			    status = 0;
	struct spi_master*	 master;
	struct device*		dev;
	char 			    spi_name[128] = {0};
	
	master = spi_busnum_to_master(af_spi_bus);
	if (master)
	{
		af_spi_master = master;
		snprintf(spi_name, sizeof(spi_name), "%s.%u", 
			dev_name(&master->dev), af_spi_cs);
		dev = bus_find_device_by_name(&spi_bus_type, NULL, spi_name);
		if (dev == NULL)
		{
			dev_err(NULL, "chipselect %d has not been used\n", af_spi_cs);
			status = -ENXIO;
			goto end1;
		}

		af_spi = to_spi_device(dev);
		if (af_spi == NULL)
		{
			dev_err(dev, "to_spi_device() error!\n");
			status = -ENXIO;
			goto end1;
		}
	}
	else
	{
		dev_err(NULL, "spi_busnum_to_master() error!\n");
		status = -ENXIO;
		goto end0;
	}
	
end1:
	put_device(dev);
end0:
	return status;
}

static u16 an41908a_cmd_read(u8 addr, u16* outdata)
{
	int 						status = 0;

	if (SPI_GPIO == g_af_info.spi_mode)
	{
		status = an41908a_spi_read8(addr, outdata);
	}
	else
	{
		struct spi_master*	         master = af_spi_master;
		struct spi_device*           spi = af_spi;
		static struct spi_message	m;
		static unsigned char        buf[8], rbuf[8];
		int                         buf_idx = 0;
		unsigned long		        flags = 0;
		
		/* check spi_message is or no finish */
		spin_lock_irqsave(&master->queue_lock, flags);
		if (m.state != NULL)
		{
			spin_unlock_irqrestore(&master->queue_lock, flags);
			dev_err(&spi->dev, 
				"\n**********%s, %s, %d line: spi_message no finish!*********\n",
				__FILE__, __func__, __LINE__);
			return -EFAULT;
		}
		spin_unlock_irqrestore(&master->queue_lock, flags);

		spi->mode = SPI_MODE_3 | SPI_LSB_FIRST; //| SPI_CS_HIGH

		memset(buf, 0, sizeof(buf));
		memset(rbuf, 0, sizeof(rbuf));
		buf[buf_idx++] = addr | 0xC0;

		hisi_gpio_write(GPIO_SPI2_CS0, 1);
		status  = spi_write_then_read(spi, buf, 1, rbuf, 2);
		hisi_gpio_write(GPIO_SPI2_CS0, 0);

		if (status)
		{
			dev_err(&spi->dev, "%s: spi_async() error!\n", __func__);
			status = -EFAULT;
		}

		*outdata = rbuf[0] | rbuf[1] << 8;

		DRV_AF_DBG("func:%s rx_buf = %#x, %#x\n", __func__, rbuf[0], rbuf[1]);
	}

	return status;
}

static int an41908a_cmd_write(u8 addr, u16 wt_data)
{
	int 	status = 0;

	if (SPI_GPIO == g_af_info.spi_mode)
	{
		status = an41908a_spi_write8(addr, wt_data);
	}
	else
	{
		struct spi_master*	master = af_spi_master;
		struct spi_device*	spi = af_spi;
		struct spi_transfer* t;
		struct spi_message*	m;
		unsigned char*		buf;    
		unsigned long		flags;
		int                 buf_idx = 0;
		int idx = g_spi_msg.msg_idx;

		g_spi_msg.msg_idx++;
		if (g_spi_msg.msg_idx > SPI_MSG_NUM - 1)
		{
			g_spi_msg.msg_idx = 0;
		}

		buf = g_spi_msg.spi_msg_array[idx].buf;
		t	= &g_spi_msg.spi_msg_array[idx].t;
		m	= &g_spi_msg.spi_msg_array[idx].m;

		/* check spi_message is or no finish */
		spin_lock_irqsave(&master->queue_lock, flags);
		if (m->state != NULL)
		{
			spin_unlock_irqrestore(&master->queue_lock, flags);
			dev_err(&spi->dev, "%s, %s, %d line: spi_message no finish!\n",
				__FILE__, __func__, __LINE__);
			return -EFAULT;
		}
		spin_unlock_irqrestore(&master->queue_lock, flags);

		spi->mode = SPI_MODE_3 | SPI_LSB_FIRST;//| SPI_CS_HIGH
		memset(buf, 0, sizeof(g_spi_msg.spi_msg_array[idx].buf));
		buf[buf_idx++] = addr & 0x3f;
		buf[buf_idx++] = wt_data & 0xff;
		buf[buf_idx++] = (wt_data >> 8) & 0xff;

		t->tx_buf	 = buf;
		t->rx_buf	 = buf;
		t->len		 = 3;
		t->cs_change = 0; //org:0
		t->speed_hz  = 1000000; //org:2000000
		t->bits_per_word = 8;

		spi_message_init(m);
		spi_message_add_tail(t, m);
		m->state = m;

		hisi_gpio_write(GPIO_SPI2_CS0, 1);
		status = spi_sync(spi, m);
		hisi_gpio_write(GPIO_SPI2_CS0, 0);

		if (status)
		{
			dev_err(&spi->dev, "%s: spi_sync() error!\n", __func__);
			status = -EFAULT;
		}
	}

	return status;
}

static int bu24036mwv_write(u16 wdata)
{
	int status = 0;

	if (SPI_GPIO == g_af_info.spi_mode)
	{
		status = bu24036mwv_spi_write8(wdata);
	}
	else
	{
		struct spi_master*	master = af_spi_master;
		struct spi_device*	spi = af_spi;
		struct spi_transfer* t;
		struct spi_message*	m;
		unsigned char*		buf;
		unsigned long		flags;
		int                 buf_idx = 0;
		int idx = g_spi_msg.msg_idx;

		g_spi_msg.msg_idx++;
		if (g_spi_msg.msg_idx > SPI_MSG_NUM - 1)
		{
			g_spi_msg.msg_idx = 0;
		}

		buf = g_spi_msg.spi_msg_array[idx].buf;
		t	= &g_spi_msg.spi_msg_array[idx].t;
		m	= &g_spi_msg.spi_msg_array[idx].m;

		/* check spi_message is or no finish */
		spin_lock_irqsave(&master->queue_lock, flags);
		if (m->state != NULL)
		{
			spin_unlock_irqrestore(&master->queue_lock, flags);
			dev_err(&spi->dev, "%s, %s, %d line: spi_message no finish!\n",
				__FILE__, __func__, __LINE__);
			return -EFAULT;
		}
		spin_unlock_irqrestore(&master->queue_lock, flags);
//		spi->mode = SPI_MODE_3;
		memset(buf, 0, sizeof(g_spi_msg.spi_msg_array[idx].buf));
		buf[buf_idx++] = (wdata >> 8) & 0xff;
		buf[buf_idx++] = wdata & 0xff;
		t->tx_buf	 = buf;
		t->rx_buf	 = buf;
		t->len		 = 2;
		t->cs_change = 0;
		t->speed_hz  = 1000000;
		t->bits_per_word = 8;
		spi_message_init(m);
		spi_message_add_tail(t, m);
		m->state = m;
		status = spi_sync(spi, m);
		if (status)
		{
			dev_err(&spi->dev, "%s: spi_sync() error!\n", __func__);
			status = -EFAULT;
		}
	}

    return status;
}

static inline int an41908a_CheckState(gpio_irq_info_t *irq_cfg)
{
	if(hisi_gpio_mode(irq_cfg->irq_gpio, GIO_IRQ_STATUS))
	{
		return 1;
	}
	
	return 0;
}

static inline void an41908a_ClrIntState(gpio_irq_info_t *irq_cfg)
{
	hisi_gpio_mode(irq_cfg->irq_gpio, GIO_IRQ_CLEAR);
}

static inline int an41908a_initIntState(gpio_irq_info_t *irq_cfg)
{
	if(irq_cfg->irq_gpio == -1)
	{
		return AF_STATUS_EFAIL;
	}
	
	if(irq_cfg->irq_type == (IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING))
	{
		hisi_gpio_mode(irq_cfg->irq_gpio, GIO_IRQ_TRIGGER_DUAL);// double edge
	}
	else if(irq_cfg->irq_type == IRQF_TRIGGER_FALLING)
	{
		hisi_gpio_mode(irq_cfg->irq_gpio, GIO_IRQ_TRIGGER_FALLING);
	}
	else if(irq_cfg->irq_type == IRQF_TRIGGER_RISING)
	{
		hisi_gpio_mode(irq_cfg->irq_gpio, GIO_IRQ_TRIGGER_RISING);
	}
	else
	{
		DRV_AF_ERR("unknow interrupt trigger way~ \n");
		return AF_STATUS_EFAIL;
	}
	
	return AF_STATUS_SOK;
}

static inline int  an41908a_init_irq(void)
{
	int retval;
	
	gpio_ext_zm.flag = 'z';
	gpio_ext_zm.irq_gpio = g_stGpio.GIO_AF_PLS2_ZM;
	gpio_ext_zm.irq_num = gpio_to_irq(gpio_ext_zm.irq_gpio);
	gpio_ext_zm.irq_type = IRQF_TRIGGER_FALLING;
	gpio_ext_fs.flag = 'f';
	gpio_ext_fs.irq_gpio = g_stGpio.GIO_AF_PLS1_FS;
	gpio_ext_fs.irq_num = gpio_to_irq(gpio_ext_fs.irq_gpio);
	gpio_ext_fs.irq_type = IRQF_TRIGGER_FALLING;
	
	printk("an41908a_init_irq==zm_irq_num:%d, fs_irq_num:%d\n", gpio_ext_zm.irq_num, gpio_ext_fs.irq_num);
	
	// Init gpio interrupt bits
	if(an41908a_initIntState(&gpio_ext_zm) != AF_STATUS_SOK)
	{
		return AF_STATUS_SOK;
	}
	if(an41908a_initIntState(&gpio_ext_fs) != AF_STATUS_SOK)
	{
		return AF_STATUS_SOK;
	}
	
	if(!g_register_irq)
	{
		g_register_irq = 1;
		g_irq_info = kzalloc(sizeof(af_iq_t), GFP_ATOMIC);
		
		g_irq_info->irq_info_zm = &gpio_ext_zm;
		g_irq_info->af_info = &g_af_ctl;
		g_irq_info->irq_info_fs = &gpio_ext_fs;
		
		retval = request_irq(gpio_ext_zm.irq_num, an41908a_busy_irq, 
			IRQF_TRIGGER_FALLING | IRQF_SHARED, DEVICE_AF_NAME, g_irq_info);
		if (retval)
		{
			printk("request_gpio_ext_zm_irq failed %d\n", retval);
			return retval;
		}
		
		retval = request_irq(gpio_ext_fs.irq_num, an41908a_busy_irq, 
			 IRQF_TRIGGER_FALLING | IRQF_SHARED, DEVICE_AF_NAME, g_irq_info);
		if (retval)
		{
			printk("request_gpio_ext_fs_irq failed %d\n", retval);
			return retval;
		}
	}
	
	return retval;
}

static inline void an41908a_irq_free(void)
{
	// free IRQ
	if(g_register_irq)
	{
		g_register_irq = 0;
		
		free_irq(gpio_ext_zm.irq_gpio, g_irq_info);
//		free_irq(gpio_ext_fs.irq_gpio, g_irq_info);
		g_irq_info = NULL;
	}
}

static inline int abf_init_irq(void)
{
	int retval = 0;

	// register irq
	gpio_ext_abf.flag = 'A';
	gpio_ext_abf.irq_gpio = g_stGpio.GIO_BOX_KEY;
	gpio_ext_abf.irq_num =  gpio_to_irq(gpio_ext_abf.irq_gpio);
	gpio_ext_abf.irq_type = IRQF_TRIGGER_FALLING;
	

	hisi_gpio_mode(gpio_ext_abf.irq_gpio, IRQF_TRIGGER_FALLING);
	hisi_gpio_mode(gpio_ext_abf.irq_gpio, GIO_IRQ_DISABLE);
	hisi_gpio_mode(gpio_ext_abf.irq_gpio, GIO_IRQ_CLEAR);
	hisi_gpio_mode(gpio_ext_abf.irq_gpio, GIO_NOIRQ_INPUT);
	DRV_ABF_DBG("\n\nABF IRQ init!!\n\n");
	
	return retval;
}

static int an41908a_out_vd(const u32 vd_num)
{
	// Set data  
	hisi_gpio_write(vd_num, 1);
	VD_DELAY;
	hisi_gpio_write(vd_num, 0);

	return AF_STATUS_SOK;
}

static int an41908a_FZInit( void )
{
	g_af_ctl.zm_steps = 0x0400|MICROXX|PULSE_ON;
	g_af_ctl.fs_steps = 0x0400|MICROXX|PULSE_ON;
	
	/*	DT1[7:0](start point wait time 0xa*8172/27Mhz) 
	  *	PWMMODE[12:8](11100=28  )  PWMRES[14:13](10=2, 30.1KHz) (Page32)
	  */
	an41908a_cmd_write(0x20, 0x5c0a);
	/*	TESTEN2[7] (enable)
	  * 	FZTEST[4:0](7: 'H' output during motor rotation)
	  */	
	an41908a_cmd_write(0x21, 0x0087);

	 //====================Zoom configure====================// 
	/*	DT2A[7:0](motor start point excitation wait time 0x3*8172/27Mhz) 
	  * 	PHMODAB[13:8](+-0 degree)
	  */
	an41908a_cmd_write(0x22, 0x0003);
	/*	PPWA[7:0]( A step numbers ) 
	  * 	PPWB[15:8]( B step numbers )
	  */		
	an41908a_cmd_write(0x23, 0xC8C8);
 	/*	PSUMAB[7:0] CCWCWAB[8](directory,0 forward)
 	  * 	BRAKEAB[9](motor brake, normal mode) ENDISAB[10](1:output on) 
 	  *	LEDB[11](LED output enable)  MICROAB[13:12](00:256 divisions)
	  */		
	an41908a_cmd_write(0x24, 0x0400|MICROXX|PULSE_ON);	// 0x0C00->0x0400
 	/*	INTCTAB[15:0](motor step cycle/ for 256-step 3*0x502/27Mhz = 142ns)
 	  * 	
	  */			
	an41908a_cmd_write(0x25, 0x0502);
	//=================================================//


	//====================Focus configure====================//
 	//Focus use CD chanel
	/*	DT2B[7:0](motor start point excitation wait time 0x3*8172/27Mhz) 
	  * 	PHMODCD[13:8](+-(0.7*0x16) degree)
	  */	
	an41908a_cmd_write(0x27, 0x1603);
	/*	PPWC[7:0]( C step numbers ) 
	  * 	PPWD[15:8]( D step numbers )
	  */		
	an41908a_cmd_write(0x28, 0xC8C8);
 	/*	PSUMCD[7:0] CCWCWCD[8](directory,0 forward)
 	  * 	BRAKEAB[9](motor brake, normal mode) ENDISAB[10](1:output on) 
 	  *	LEDA[11](LED output enable)  MICROAB[13:12](00:256 divisions)
	  */			
	an41908a_cmd_write(0x29, 0x0400|MICROXX|PULSE_ON);	// 0x0C00->0x0400
 	/*	INTCTCD[15:0](motor step cycle/ for 256-steop 3*0x400/27Mhz = 114ns)
 	 * 	
	 */		
	an41908a_cmd_write(0x2A, 0x0400);
	//=================================================//
	
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);

	return 0;
}


static int an41908a_IrisInit( void )
{
	//Set Iris Target[9:0]
	an41908a_cmd_write(0x00, 0x0000);

	/* 	LPF1[1:0](5200HZ) LPF2[3:2](5200HZ) DEC_AVE[4](8)
	  *	AS_FLT_OFF[5](0) LPF_FC[8:6](1600HZ)
	  *	DGAIN[15:9] = 0x30, gain= 4, 15.0dB 
	  */
	an41908a_cmd_write(0x01, 0x608A); 
	/*	IRIS_CALC_NR[3:0](Disable) IRIS_ROUND[7:4](+-1LSB)
	  *	PID_ZERO[11:8](35HZ/35HZ) PID_POLE[15:9](1200HZ)
	  */
	an41908a_cmd_write(0x02, 0x66F0);
	/*	ARW[3:0](12bit) LMT_ENB[4](enable) PWM_FLT_OFF[5](disable)
	  *	PWM_LPF_FC[8:6](900HZ) PWM_IRIS[11:9](210kHz)
	  *	DT_ADJ_IRIS[13:12](Standard correction)
	  */
	an41908a_cmd_write(0x03, 0x0E10); //org: 0x0E73, goli.milesight modify
	/*	HALL_BIAS_DAC[7:0](0x25) I=REF/Rref*(value/8)=1.26V/Rref*(value/8)
	  *	HALL_OFFSET_DAC[15:8] amp=AVDD3/256*(0x7e-128)
	  */
	an41908a_cmd_write(0x04, 0x804C); // 0x804c-> 0x7473 I = 126uA/8*76=1.197mA
	/*	TGT_LPF_FC[3:0](40HZ) TGT_FLT_OFF[4](disable) 
	  *	PID_INV[5](Non-inverting) HALL_GAIN[11:8](67.1)
	  *	AAF_FC[12](6.85kHZ)
	  */		
	an41908a_cmd_write(0x05, 0x0504);
	/* TGT_IN_TEST[9:0](0) DUTY_TEST[10](Disable, Normal operation))*/
	an41908a_cmd_write(0x0A, 0x0080);
	/*	PDWNB[10](iris control enable) ASWMODE[4:3](Normal mode) 
	  *	ADC_TEST[11](Normal operation) PID_CLIP[15:12](duty:100%) TESTEN1[7](enable)
	  */
	an41908a_cmd_write(0x0B, 0x0480);
	/* TGT_UPDATE[7:0](0) AVE_SPEED[12:8]((12*512+1)/3.375us) */
	an41908a_cmd_write(0x0E, 0x0C00);
	
	an41908a_out_vd(g_stGpio.GIO_AF_VD_IS);

	return 0;
}

//1 Autonomous mode (Cache method). 
/**********************************************************************
[u-step mode, the pre-excitation=ON, the post-excitation=ON, 1 rotation]
command 1:	Command-reset. 												----- CMD_RS
command 2:	Released from Command-reset and Stand-by.					----- CMD_RS+STB
command 3:	Set the clock supplied to the main logic, set to clock=ON. 	 		----- CLK_DIV+CLK_EN
command 4:	Released from STM-reset.										----- STM_RS
command 5:	Set the control mode of stepping motor.(Autonomous mode) 	   	----- A/B_CTL
command 6:	Set PWM chopping frequency for stepping motor driver.			----- Chopping
			Set the mode of Cache register. 								----- CacheM
command 7:	Set the mode of stepping motor control.(u-step) 					----- A/B_Mode
			Set the output signal from STATE pin							----- A/B_SEL
			Set the different output voltage									----- A/B_different_output_voltage
command 8,9:	Set the frequency for stepping motor rotation					----- A/B_Cycle
command 10:	Set ON/OFF of the pre-excitation.(ON)						----- A/B_BEXC
				Set time of the pre-excitation. 								----- A/B_BSL
				Set ON/OFF of the post-excitation.(ON)						----- A/B_AEXC
				Set time of the post-excitation.								----- A/B_ASL
command 11:	Set Power ON/OFF of stepping motor driver					----- A/B_PS
command 12:	Set the excitation/un excitation of driver(excitation) 		 	----- A/B_EN
				Set the rotating direction									----- A/B_RT
				Set the amount of rotation									----- A/B_Pulse
**********************************************************************/
static int bu24036mwv_zf_init(driver_lens_type_e driver_type)
{
	g_af_info.driver_lens_type = driver_type;
	
	//4 cmd1: Reset CMD_RS[0], STM_RS[1], STB[4]
	bu24036mwv_write(COMM_RST | RST_ON | STM_RST_ON | STB_ON);
	af_msleep(50);
	
	//4 cmd2: Release CMD_RS[0], Stand-by STB[4].
	bu24036mwv_write(COMM_RST | RST_OFF | STM_RST_ON | STB_OFF);
	
	//4 cmd3: Set the clock supplied to the main logic, set to clock=ON, cache number=2.
	/* Set CLK_EN[4] and CLK_DIV[3:0], 5-MODE[5], CacheM[7], Chopping[9:8]
	  * CLK_DIV = (0 << 0): FCLK = 27M, fmain = FCLK*CLK_DIV in[17.5M,28M]
	  * CLK_EN = (1 << 4): enable man clock
	  */
	bu24036mwv_write(COMM_CLK_CFG);
	
	//4 cmd4:  Released from STM-reset, out off stand-by.
	bu24036mwv_write(COMM_RST | RST_OFF | STM_RST_OFF | STB_OFF);

	//4 cmd5: Set the control mode of stepping motor.(Autonomous mode)
	/* Set CHA and CHB B_ANSEL[7],A_ANSEL[6], Edge[5], B_CTL[1],A_CTL[0]
	  * A/B_ANSEL = 0: cache mode, 
	  * Edge = 0, only use in Clock in mode.
	  * A/B_CTL = 0, Autonomous mode.
	  */
	bu24036mwv_write(STEPPER_CFG | ST_A_AUTO | ST_B_AUTO | ST_A_CACHE | ST_B_CACHE);
	
	//4 cmd6: Set PWM chopping frequency and cache mode
	/* Chopping = (0x2 << 8): PWM = fmain/Chopping in [120k,200k], PWM = 27M/(32*6) =  140625 HZ
	  * 5-Mode = (0 << 5): volatge driver, use for control Iris.
	  * CacheM = (0 << 7): cache number = 2
	  */
	bu24036mwv_write(COMM_CLK_CFG | PWM_DIV_6); // set chopping after clock enable.

	//4 cmd7: Set the mode of stepping motor control, STATE and output voltage.
	/* CHA and CHB u-step set and state set.
	  * A/B_Mode[11:10], A/B_Sel[9:7], A/B_diff_out_voltage[6:0]
	  * A/B_Mode = 0: 1/4, use 1024 u-step to run.
	  * A/B_Sel = 2: STATEx1 = busy(use to update cache), STATEx2 = ACT(use judge if motor is running)
	  * A/B_diff_out_voltage = 0x7f: DVDD=3.3V, MVCC=5V, OUT12 = DVDD*2/128*0x7f = 5V.(max=MVCC)
	  */
	bu24036mwv_write(FS_MODE | U_STEP_MODE | STATE_BUSY_ACT); //set chA configure.
	bu24036mwv_write(ZM_MODE | U_STEP_MODE | STATE_BUSY_ACT); //set chB configure.
	bu24036mwv_write(FS_MODE | U_STEP_MODE | STATE_BUSY_ACT | VOLT_4_5v); //set chA output voltage.  
	bu24036mwv_write(ZM_MODE | U_STEP_MODE | STATE_BUSY_ACT | VOLT_4_5v); //set chB output voltage.

	if(g_af_info.driver_lens_type == DRIVER_LENS_ABF || g_af_info.driver_lens_type == DRIVER_LENS_BOX)
		bu24036mwv_write(ZM_MODE | BOX_U_STEP_MODE | STATE_BUSY_ACT | VOLT_4_5v); //set chA output voltage.
	else
		bu24036mwv_write(ZM_MODE | U_STEP_MODE | STATE_BUSY_ACT | VOLT_4_5v); //set chB output voltage.

	//4 cmd8: Set the frequency for stepping motor rotation.
	/* CHA and CHB  pps set.
	  * A/B_Cycle_L[5:0], A/B_Cycle_H[13:6]
	  * PPS = fmain/(128*x), x = fmain/128/pps
	  */
	bu24036mwv_write(FS_SPEED_H | CYCLE_H(800));
	bu24036mwv_write(FS_SPEED_L | CYCLE_L(800));
	bu24036mwv_write(ZM_SPEED_H | CYCLE_H(800));
	bu24036mwv_write(ZM_SPEED_L | CYCLE_L(800));

	//4 cmd9: Set the stop_pos and power for stepping motor rotation.
	/* CHA and CHB  start pos and stop pos set, power enable.
	  * A/B_Cycle_L[5:0], A/B_Cycle_H[13:6]
	  * PPS = fmain/(128*x), x = fmain/128/pps
	  */
	bu24036mwv_write(CHN12_INIT_POS);
	bu24036mwv_write(CHN34_INIT_POS);
	bu24036mwv_write(CHN12_STOP_POS);
	bu24036mwv_write(CHN34_STOP_POS);

	//4 cmd10: Set pre-excitation and post-excitation.
	/* CHA and CHB  excitation set.
	  * A/B_BEXC[7], A/B_BSL[6], A/B_AEXC[7], A/B_ASL[6]
	  */
	bu24036mwv_write(CHN12_EXC_ON1);
	bu24036mwv_write(CHN34_EXC_ON1);

	//4 cmd11: Set Power ON/OFF of stepping motor driver.
	/* CHA and CHB  power on, excitation on, operation on.
	  * A/B_BEXC[7], A/B_BSL[6], A/B_AEXC[7], A/B_ASL[6]
	  */
	bu24036mwv_write(CHN12_STOP_POS | PS12_ON);
	bu24036mwv_write(CHN34_STOP_POS | PS34_ON);

	bu24036mwv_write(STEPPER_FOCUS | ST_OFF);
	bu24036mwv_write(STEPPER_ZOOM | ST_OFF);

	return 0;
}

static int bu24036mwv_iris_init( void )
{
	/************************configure iris******************************/
	//4 stg1. Set channel 5 output voltage.
	bu24036mwv_write(COMM_CLK_CFG | PWM_DIV_6 | CHN5_MODE_VOLATGE);

	//4 stg2. Set frequency for channel 5 voltage.
	/* Channel 5 freq = Fclk/(128 * (2^n))
	  * So, when we use 24M osc, pwm = 100KHZ, n = 0x1
	  */
	bu24036mwv_write(CHN5_CTRL | CHN5_PWM_N(1) | CHN5_HIZ);

	//4 stg3. Set channel 5 control type.
	bu24036mwv_write(EXT_CTRL | CHN56_REG_REG);
	bu24036mwv_write(CHN5_CTRL | CHN5_PWM_N(1) | CHN5_FORWARD);

	return 0;
}

static int bu24036mwv_zf_run(
	u16 zm_pps, s32 zm_dist, u16 fs_pps, s32 fs_dist)
{
	u8 zm_in = (zm_dist < 0) ? 1 : 0;
	u8 fs_far = (fs_dist < 0) ? 1 : 0;
	u16 zm_data, fs_data;
	u8 b_zf_run = 0;
	unsigned long flags = 0;
//	static int steps_sum_zm = 0;
//	static int steps_sum_fs = 0;
	
	// Set af is running
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.fs_busy = 1;
	g_af_ctl.zm_busy = 1;
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	g_af_ctl.zm_idx = zm_dist;
	g_af_ctl.fs_idx = fs_dist;
	g_af_ctl.dist_zm = AF_ABS(zm_dist * DISTANCE2USTEP) ;  //8-step;
	g_af_ctl.dist_fs = AF_ABS(fs_dist * DISTANCE2USTEP) ;  //8-step;

	// caculate INTCTxx and PUSMxx
	g_af_ctl.vd_PSUM_zm = PULSE_MAX;
	g_af_ctl.vd_PSUM_fs = PULSE_MAX;
	g_af_ctl.vd_num_zm = g_af_ctl.dist_zm/g_af_ctl.vd_PSUM_zm;
	g_af_ctl.vd_num_fs = g_af_ctl.dist_fs/g_af_ctl.vd_PSUM_fs;
	if(g_af_ctl.dist_zm > 0 && g_af_ctl.dist_fs > 0)
	{
		b_zf_run = 1;
		if(g_af_ctl.vd_num_zm == 0)
			g_af_ctl.vd_PSUM_zm = g_af_ctl.dist_zm;
		if(g_af_ctl.vd_num_fs == 0)
			g_af_ctl.vd_PSUM_fs = g_af_ctl.dist_fs;	
	}
	else if(g_af_ctl.dist_zm > 0 && g_af_ctl.dist_fs == 0)
	{
		g_af_ctl.vd_num_fs = -1;
		if(g_af_ctl.vd_num_zm == 0)
			g_af_ctl.vd_PSUM_zm = g_af_ctl.dist_zm;
	}
	else if(g_af_ctl.dist_zm == 0 && g_af_ctl.dist_fs > 0)
	{
		g_af_ctl.vd_num_zm = -1;
		if(g_af_ctl.vd_num_fs == 0)
			g_af_ctl.vd_PSUM_fs = g_af_ctl.dist_fs;
	}
	else
	{
		spin_lock_irqsave(&af_spinlock, flags);
		g_af_ctl.fs_busy = 0;
		g_af_ctl.zm_busy = 0;
		spin_unlock_irqrestore(&af_spinlock, flags);
		return 0;
	}

	// set pps, dir, pulse and start stepper.
	zm_data = STEPPER_ZOOM | ST_ON | PULSE_VAL(g_af_ctl.vd_PSUM_zm);
	fs_data = STEPPER_FOCUS | ST_ON | PULSE_VAL(g_af_ctl.vd_PSUM_fs);
	if(!zm_in)
		zm_data |= ST_REVERSE;
	if(!fs_far)
		fs_data |= ST_REVERSE;

	bu24036mwv_write(FS_SPEED_H | CYCLE_H(fs_pps));
	bu24036mwv_write(FS_SPEED_L | CYCLE_L(fs_pps));
	bu24036mwv_write(ZM_SPEED_H | CYCLE_H(zm_pps));
	bu24036mwv_write(ZM_SPEED_L | CYCLE_L(zm_pps));

	while((g_af_ctl.vd_num_zm >= 0) || (g_af_ctl.vd_num_fs >= 0))
	{
		if(g_af_ctl.vd_num_zm >= 0)
		{
			if(!hisi_gpio_read(g_stGpio.GIO_BU24_B2)) //Goli.milesight modify
			{
				bu24036mwv_write(zm_data);
				g_af_ctl.vd_num_zm--;
				if(g_af_ctl.vd_num_zm == 0)
				{
					g_af_ctl.vd_PSUM_zm = g_af_ctl.dist_zm%PULSE_MAX;
					if(g_af_ctl.vd_PSUM_zm == 0)
					{
						g_af_ctl.vd_num_zm--;
					}
					else
					{
						zm_data &= 0xfc00;
						zm_data |= PULSE_VAL(g_af_ctl.vd_PSUM_zm);
					}
				}	
			}	
		}

		if(g_af_ctl.vd_num_fs >= 0)
		{
			if(!hisi_gpio_read(g_stGpio.GIO_BU24_A2)) //Goli.milesight modify
			{
				bu24036mwv_write(fs_data);
				g_af_ctl.vd_num_fs--;
				if(g_af_ctl.vd_num_fs == 0)
				{
					g_af_ctl.vd_PSUM_fs = g_af_ctl.dist_fs%PULSE_MAX;
					if(g_af_ctl.vd_PSUM_fs == 0)
					{
						g_af_ctl.vd_num_fs--;
					}
					else
					{
						fs_data &= 0xfc00;
						fs_data |= PULSE_VAL(g_af_ctl.vd_PSUM_fs);
					}
				}
			}
		}
		
		if((g_af_ctl.vd_num_zm >= 0) || (g_af_ctl.vd_num_fs >= 0))
		{
			af_msleep(1);
		}
	}
	
	if(!b_zf_run)
	{
		bu24036mwv_wait_zm_stop();
		bu24036mwv_wait_fs_stop();
//		steps_sum_zm += AF_ABS(zm_dist);
//		if(steps_sum_zm >= 20)
//		{
//			steps_sum_zm = 0;
//			bu24036mwv_write(STEPPER_ZOOM | ST_OFF);
//		}	
//		steps_sum_fs += AF_ABS(fs_dist);
//		if(steps_sum_fs >= 20)
//		{
//			steps_sum_fs = 0;
//			bu24036mwv_write(STEPPER_FOCUS | ST_OFF);
//		}
	}
	
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_info.zm_pos += zm_dist;
	g_af_info.fs_pos += fs_dist;	
	g_af_ctl.fs_busy = 0;
	g_af_ctl.zm_busy = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);

	return 0;
}

int box_piris_run(u16 pps, s32 dist)
{
	u8 piris_far = (dist < 0) ? 0 : 1;
	u16 piris_data;
	//u8 b_zf_run = 0;

	if(bu24036mwv_check_run())
		return -1;

	// Set af is running
	g_af_ctl.zm_busy = 1;

	//g_af_ctl.fs_idx = dist;
	g_af_ctl.dist_zm = AF_ABS(dist * BOX_DISTANCE2USTEP);  

	// caculate INTCTxx and PUSMxx
	g_af_ctl.vd_PSUM_zm = BOX_PULSE_MAX;
	g_af_ctl.vd_num_zm = g_af_ctl.dist_zm/g_af_ctl.vd_PSUM_zm;
	if(g_af_ctl.dist_zm > 0)
	{
		//b_zf_run = 1;
		if(g_af_ctl.vd_num_zm == 0)
			g_af_ctl.vd_PSUM_zm = g_af_ctl.dist_zm;	
	}
	else
	{
		g_af_ctl.zm_busy = 0;
		return 0;
	}

	// set pps, dir, pulse and start stepper.
	piris_data = STEPPER_ZOOM | ST_ON | BOX_PULSE_VAL(g_af_ctl.vd_PSUM_zm);

	if(!piris_far)
		piris_data |= ST_REVERSE;

	bu24036mwv_write(ZM_SPEED_H | CYCLE_H(pps));
	bu24036mwv_write(ZM_SPEED_L | CYCLE_L(pps));
	
	while(g_af_ctl.vd_num_zm >= 0)
	{
		if(g_af_ctl.vd_num_zm >= 0)
		{
			if(!hisi_gpio_read(g_stGpio.GIO_BU24_B2)) //Goli.milesight modify
			{
				bu24036mwv_write(piris_data);
				g_af_ctl.vd_num_zm--;
				if(g_af_ctl.vd_num_zm == 0)
				{
					g_af_ctl.vd_PSUM_zm = g_af_ctl.dist_zm%BOX_PULSE_MAX;
					if(g_af_ctl.vd_PSUM_zm == 0)
					{
						g_af_ctl.vd_num_zm--;
					}
					else
					{
						piris_data &= 0xfc00;
						piris_data |= BOX_PULSE_VAL(g_af_ctl.vd_PSUM_zm);
					}
				}
			}
		}
	}

	g_af_ctl.zm_busy = 0;

	return 0;
}
EXPORT_SYMBOL(box_piris_run);

static int an41908a_zoom_run(u16 pps, int distance)
{
	unsigned long flags = 0;
	u8 zm_in = (distance > 0) ? 1 : 0;
	
	an41908_up = 0;  //only for double zoom
	if(distance == 0)
		return 0;
		
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_mode = AF_MODE_ZOOM;
	g_af_ctl.zm_busy = 1;
	g_af_ctl.zm_irq_num = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);
	g_af_ctl.zm_idx = distance;
	g_af_ctl.dist_zm = AF_ABS(distance) * DISTANCE_TO_MICRO;  //8-step -> 256-step;

	// Recaculate PPS and step.
	g_vd_PSUMxx_zm = VD_PSUMXX(pps);
	g_vd_INTCTxx_zm = VD_INTCTXX(g_vd_PSUMxx_zm);

	an41908a_cmd_read( AN41908A_ZOOM_STEPS_REG,  (u16*)&g_af_ctl.zm_steps);
	// caculate INTCTxx and PUSMxx
	g_af_ctl.vd_PSUM_zm = g_vd_PSUMxx_zm;
	g_af_ctl.vd_num_zm = g_af_ctl.dist_zm/(g_af_ctl.vd_PSUM_zm * PSUM_TO_STEP);
	
	// set pps
	an41908a_cmd_write(AN41908A_ZOOM_PPS_REG, (u16)g_vd_INTCTxx_zm);	// set pps
	// set step
	g_af_ctl.zm_steps &= 0xFE00;
	if(!zm_in)
		g_af_ctl.zm_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
	if(g_af_ctl.vd_num_zm)
	{
		g_af_ctl.zm_steps |= g_af_ctl.vd_PSUM_zm;
	}
	else
	{
		g_af_ctl.zm_steps |= (g_af_ctl.dist_zm%(g_af_ctl.vd_PSUM_zm * PSUM_TO_STEP))/PSUM_TO_STEP;
	}
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
	// Clean irq flag,  output a AF_VD_FZ pluse to start af.
	g_af_ctl.fs_irq_dn = 0;
	g_af_ctl.zm_irq_dn = 0;
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	DRV_AF_DBG("%s: pps: %d, dist: %d, PSUMxx: %d, "
		"INTCTxx: %d, vd_num: %d, zm_irq_num:%d \n",
		__func__, pps, g_af_ctl.dist_zm/DISTANCE_TO_MICRO, 
		g_af_ctl.vd_PSUM_zm, g_vd_INTCTxx_zm, 
		g_af_ctl.dist_zm/(g_af_ctl.vd_PSUM_zm * PSUM_TO_STEP), 
		g_af_ctl.zm_irq_num);

	return 0;
}

static int an41908a_focus_run(u16 pps, int distance)
{
	unsigned long flags = 0;
	u8  fs_near = (distance > 0) ? 1 : 0;

	if(distance == 0)
		return 0;
	an41908_up = 0;  //only for double zoom
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_mode = AF_MODE_FOCUS;
	g_af_ctl.fs_busy = 1;
	g_af_ctl.fs_irq_num = 0;	
	spin_unlock_irqrestore(&af_spinlock, flags);
	g_af_ctl.fs_idx = distance;
	g_af_ctl.dist_fs = AF_ABS(distance) * (DISTANCE_TO_MICRO/g_af_info.STEP_MICRO);  //8-step -> 256-step;

	// Recaculate PPS and step.
	g_vd_PSUMxx_fs = VD_PSUMXX(pps);
	g_vd_INTCTxx_fs = VD_INTCTXX(g_vd_PSUMxx_fs);
	
	an41908a_cmd_read( AN41908A_FOCUS_STEPS_REG,  (u16*)&g_af_ctl.fs_steps);
	// caculate INTCTxx and PUSMxx
	g_af_ctl.vd_PSUM_fs = g_vd_PSUMxx_fs;
	g_af_ctl.vd_num_fs = g_af_ctl.dist_fs/(g_af_ctl.vd_PSUM_fs * PSUM_TO_STEP);

	// set pps
	an41908a_cmd_write(AN41908A_FOCUS_PPS_REG, (u16)g_vd_INTCTxx_fs);	// set pps
	// set step
	g_af_ctl.fs_steps &= 0xFE00;
	if(!fs_near)
		g_af_ctl.fs_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
	if(g_af_ctl.vd_num_fs)
	{
		g_af_ctl.fs_steps |= g_af_ctl.vd_PSUM_fs;
	}
	else
	{
		g_af_ctl.fs_steps |= (g_af_ctl.dist_fs%(g_af_ctl.vd_PSUM_fs * PSUM_TO_STEP))/PSUM_TO_STEP;
	}
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);

	// Clean irq flag,  output a AF_VD_FZ pluse to start af.
	g_af_ctl.fs_irq_dn = 0;
	g_af_ctl.zm_irq_dn = 0;
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	
	DRV_AF_DBG("%s: pps: %d, dist: %d, PSUMxx: %d, "
		"INTCTxx: %d, vd_num: %d, fs_irq_num:%d \n",
		__func__, pps, g_af_ctl.dist_fs/DISTANCE_TO_MICRO, 
		g_af_ctl.vd_PSUM_fs, g_vd_INTCTxx_fs, 
		g_af_ctl.dist_fs/(g_af_ctl.vd_PSUM_fs * PSUM_TO_STEP),
		g_af_ctl.fs_irq_num);
	
	return 0;
}

static int an41908a_zoom_stop(void)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.zm_busy = 1;
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	an41908a_cmd_read( AN41908A_ZOOM_STEPS_REG,  (u16*)&g_af_ctl.zm_steps);
	g_af_ctl.zm_steps &= 0xFF00;
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.zm_busy = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	return 0;
}

int bu24036mwv_zoom_stop(void)
{
	bu24036mwv_write(STEPPER_ZOOM | ST_OFF);
	return 0;
}
EXPORT_SYMBOL(bu24036mwv_zoom_stop);

static int an41908a_focus_stop(void)
{
	unsigned long flags = 0;
	
//	printk("==an41908a_focus_stop===\n");
	
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.fs_busy = 1;
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	an41908a_cmd_read( AN41908A_FOCUS_STEPS_REG, (u16*)&g_af_ctl.fs_steps);
	g_af_ctl.fs_steps &= 0xFF00;
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);

	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.fs_busy = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);

	return 0;
}

static int bu24036mwv_focus_stop(void)
{
	bu24036mwv_write(STEPPER_FOCUS | ST_OFF);
	return 0;
}

static int an41908a_set_aperture(u16 aperture_idx)
{
	//IrsTGT range:0x0000~0x03FF
	if ( aperture_idx > 0x03ff )
	{
		aperture_idx = 0x03ff ;
	}
	
	an41908a_cmd_write(0x00, aperture_idx);  //Set Iris Target
	//output a VD_IS pluse
	an41908a_out_vd(g_stGpio.GIO_AF_VD_IS);
	
	return 0;
}

static int bu24036mwv_set_aperture(u16 aperture_idx)
{
	return 0;
}


static int an41908a_set_ircut(u8 enable)
{
	return 0;
}

static int bu24036mwv_set_ircut(u8 enable)
{
	return 0;
}


static int an41908a_set_zm_pi(u16 z_pi,u16 speed)
{
	PI_status zpi, zpi_init;
	int test_time = 0;
	unsigned long flags = 0;

	// Set af is running
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_mode = AF_MODE_NONE;
	g_af_ctl.zm_busy = 1;
	g_af_ctl.zm_irq_num = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	an41908a_cmd_read( AN41908A_ZOOM_STEPS_REG,  (u16*)&g_af_ctl.zm_steps);
	
	// set pps
	an41908a_cmd_write(AN41908A_ZOOM_PPS_REG, (u16)g_vd_PSUMxx_zm);	// set pps

	// set Pi on
	g_af_ctl.zm_steps &= 0xFE00;
	g_af_ctl.zm_steps |= PULSE_ON;	// open pi control switch
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	af_msleep(1000);
	zpi_init = zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM); //Goli.milesight modify

	g_af_ctl.zm_steps &= 0xFE00;
	g_af_ctl.zm_steps |= speed;
	if (PI_DN == g_af_info.zpi_status)	//special for LENS DF019N
	{
		if (zpi == PI_DN) // near point, move to the far point
		{
			g_af_ctl.zm_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	else
	{
		if (zpi == PI_EN) // near point, move to the far point
		{
			g_af_ctl.zm_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
	
	//2 Run continue steps
	while (zpi_init == zpi)
	{
		g_af_ctl.zm_irq_dn = 0;
		
		// output a AF_VD_FZ pluse
		an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		
		// wait for front opration finish.
		an41908a_wait_zm_stop();
	
		// check PI state
		zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM); //Goli.milesight modify
		if(test_time++ %500 == 0)
			DRV_AF_DBG("zpi_init = %d, zpi2 = %d \n", zpi_init, zpi);
	}
	
	//2  stop zoom
	g_af_ctl.zm_steps &= 0xFF00;
	g_af_ctl.zm_steps &= ~PULSE_ON;	// close pi control switch
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);

	//output a AF_VD_FZ pluse
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_cfg.zm_idx = 0;
	g_af_ctl.zm_busy = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);
	return 0;
}

static int an41908a_set_fs_pi(u16 f_pi,u16 speed)
{
	PI_status fpi, fpi_init;
	int test_time = 0;
	unsigned long flags = 0;
	
	// Set af is running
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_mode = AF_MODE_NONE;
	g_af_ctl.fs_busy = 1;
	g_af_ctl.fs_irq_num = 0;	
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	an41908a_cmd_read( AN41908A_FOCUS_STEPS_REG,  (u16*)&g_af_ctl.fs_steps);
	
	// set pps
	an41908a_cmd_write(AN41908A_FOCUS_PPS_REG, (u16)g_vd_PSUMxx_fs);	// set pps
	
	// set Pi on
	g_af_ctl.fs_steps &= 0xFE00;
	g_af_ctl.fs_steps |= PULSE_ON;	// open pi control switch	
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	af_msleep(1000);
	fpi_init = fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS); //Goli.milesight modify
	
	g_af_ctl.fs_steps &= 0xFE00;
	g_af_ctl.fs_steps |= speed;
	if (PI_DN == g_af_info.fpi_status)  //special for LENS DF019N
	{
		if (fpi == PI_DN) // near point, move to the far point
		{
			g_af_ctl.fs_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	else
	{
		if (fpi == PI_EN) // near point, move to the far point
		{
			g_af_ctl.fs_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	
	//2 Run continue steps
	while (fpi_init == fpi)
	{
		g_af_ctl.fs_irq_dn = 0;
		
		// output a AF_VD_FZ pluse
		an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		
		// wait for front opration finish.
		an41908a_wait_fs_stop();
	
		// check PI state
		fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS); //Goli.milesight modify
		if(test_time++ %500 == 0)
			DRV_AF_DBG("fpi_init = %d, fpi2 = %d \n", fpi_init, fpi);
	}
	
	//2  stop focus
	g_af_ctl.fs_steps &= 0xFF00;
	g_af_ctl.fs_steps &= ~PULSE_ON;	// close pi control switch
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	
	//output a AF_VD_FZ pluse
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);

	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_cfg.fs_idx = 0;
	g_af_ctl.fs_busy = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	return 0;
}


//#define AF_DEBUG
static int an41908a_set_zf_pi(u16 z_pi, u16 f_pi,u16 speed)
{
	PI_status fpi, fpi_init, zpi, zpi_init;
	int	bzm_pi = 0, bfs_pi = 0;
	int test_time = 0, fcnt = 0, zcnt = 0, ftmp = 0, ztmp = 0;
	unsigned int zpps = 0, fpps = 0;
	unsigned long flags = 0;
	
	hisi_gpio_write(g_stGpio.GIO_LENS_PI_POWER_EN, LENS_LIGHT_ON); //open lens optocoupler
	// Set af is running
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_mode = AF_MODE_NONE;
	g_af_ctl.fs_busy = 1;
	g_af_ctl.zm_busy = 1;
	g_af_ctl.zm_irq_num = 0;
	g_af_ctl.fs_irq_num = 0;	
	spin_unlock_irqrestore(&af_spinlock, flags);
	
	an41908a_cmd_read( AN41908A_FOCUS_STEPS_REG,  (u16*)&g_af_ctl.fs_steps);
	an41908a_cmd_read( AN41908A_ZOOM_STEPS_REG,  (u16*)&g_af_ctl.zm_steps);
	
	// set pps
	an41908a_cmd_write(AN41908A_FOCUS_PPS_REG, (u16)g_vd_PSUMxx_fs);	// set pps
	an41908a_cmd_write(AN41908A_ZOOM_PPS_REG, (u16)g_vd_PSUMxx_zm);	// set pps

	// set Pi on, check initial PI value
	g_af_ctl.fs_steps &= 0xFE00;
	g_af_ctl.fs_steps |= PULSE_ON;	// open pi control switch	
	g_af_ctl.zm_steps &= 0xFE00;
	g_af_ctl.zm_steps |= PULSE_ON;	// open pi control switch	
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
	af_msleep(1000);

	// added by yangyang 20220718 to decrease the lens init time.
	// Recaculate PPS and step.
	zpps = VD_INTCTXX(VD_PSUMXX(800));
	fpps = VD_INTCTXX(VD_PSUMXX(800));
	an41908a_cmd_write(AN41908A_ZOOM_PPS_REG, (u16)zpps);
	an41908a_cmd_write(AN41908A_FOCUS_PPS_REG, (u16)fpps);
	g_af_ctl.af_exit = 0;
	fcnt = abs(g_af_info.fs_near_bd - g_af_info.fs_far_bd) * (DISTANCE_TO_MICRO / g_af_info.STEP_MICRO);
	zcnt = abs(g_af_info.zm_in_bd - g_af_info.zm_out_bd) * (DISTANCE_TO_MICRO / g_af_info.STEP_MICRO);
	// Fast run to High level side
	ftmp = 0;
	ztmp = 0;
	bzm_pi = 0;
	bfs_pi = 0;
	fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS);
	zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM);
	if (PI_DN == g_af_info.fpi_status) {
		g_af_ctl.fs_steps = (fpi == PI_DN) ? (g_af_ctl.fs_steps | 0x0100) : (g_af_ctl.fs_steps & 0xFE00);
	} else {
		g_af_ctl.fs_steps = (fpi == PI_DN) ? (g_af_ctl.fs_steps & 0xFE00) : (g_af_ctl.fs_steps | 0x0100);
	}
	if (PI_DN == g_af_info.zpi_status) {
		g_af_ctl.zm_steps = (zpi == PI_DN) ? (g_af_ctl.zm_steps | 0x0100) : (g_af_ctl.zm_steps & 0xFE00);
	} else {
		g_af_ctl.zm_steps = (zpi == PI_DN) ? (g_af_ctl.zm_steps & 0xFE00) : (g_af_ctl.zm_steps | 0x0100);
	}
	g_af_ctl.fs_steps |= PULSE_ON;    // open pi control switch
	g_af_ctl.zm_steps |= PULSE_ON;    // open pi control switch
	g_af_ctl.fs_steps |= 0x80;
	g_af_ctl.zm_steps |= 0x80;
	an41908a_cmd_write(AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_cmd_write(AN41908A_ZOOM_STEPS_REG,  g_af_ctl.zm_steps);
	while (!bzm_pi || !bfs_pi) {
		g_af_ctl.zm_irq_dn = 0;
		g_af_ctl.fs_irq_dn = 0;
		if (!bfs_pi) {
			fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS);
			if (fpi != PI_DN) {
				bfs_pi = 1;
				g_af_ctl.fs_steps &= 0xFF00;
				an41908a_cmd_write(AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
			} else {
				ftmp += 0x80;
			}
		}
		if (!bzm_pi) {
			zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM);
			if (zpi != PI_DN) {
				bzm_pi = 1;
				g_af_ctl.zm_steps &= 0xFF00;
				an41908a_cmd_write(AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
			} else {
				ztmp += 0x80;
			}
		}
		if (ftmp >= fcnt || ztmp >= zcnt) {
			DRV_AF_ERR("Find PI failed, ftmp: %d, fcnt: %d, ztmp: %d, zcnt: %d\n", ftmp, fcnt, ztmp, zcnt);
			return -1;
		}
		if (g_af_ctl.af_exit == 1) {
			DRV_AF_DBG("find zf_pi failed or try to exit the set_zf_pi %d\n", g_af_ctl.af_exit);
			return -1;
		}
		an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		af_msleep(20);
		if (!bfs_pi) {
			an41908a_wait_fs_stop();
		}
		if (!bzm_pi) {
			an41908a_wait_zm_stop();
		}
	}

	// Fast run to Low level side
	ftmp = 0;
	ztmp = 0;
	bzm_pi = 0;
	bfs_pi = 0;
	fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS);
	zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM);
	if (PI_DN == g_af_info.fpi_status) {
		g_af_ctl.fs_steps = (fpi == PI_DN) ? (g_af_ctl.fs_steps | 0x0100) : (g_af_ctl.fs_steps & 0xFE00);
	} else {
		g_af_ctl.fs_steps = (fpi == PI_DN) ? (g_af_ctl.fs_steps & 0xFE00) : (g_af_ctl.fs_steps | 0x0100);
	}
	if (PI_DN == g_af_info.zpi_status) {
		g_af_ctl.zm_steps = (zpi == PI_DN) ? (g_af_ctl.zm_steps | 0x0100) : (g_af_ctl.zm_steps & 0xFE00);
	} else {
		g_af_ctl.zm_steps = (zpi == PI_DN) ? (g_af_ctl.zm_steps & 0xFE00) : (g_af_ctl.zm_steps | 0x0100);
	}
	g_af_ctl.fs_steps |= PULSE_ON;    // open pi control switch
	g_af_ctl.zm_steps |= PULSE_ON;    // open pi control switch
	g_af_ctl.fs_steps |= 0x80;
	g_af_ctl.zm_steps |= 0x80;
	an41908a_cmd_write(AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_cmd_write(AN41908A_ZOOM_STEPS_REG,  g_af_ctl.zm_steps);
	while (!bzm_pi || !bfs_pi) {
		g_af_ctl.zm_irq_dn = 0;
		g_af_ctl.fs_irq_dn = 0;
		if (!bfs_pi) {
			fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS);
			if (fpi != PI_EN) {
				bfs_pi = 1;
				g_af_ctl.fs_steps &= 0xFF00;
				an41908a_cmd_write(AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
			} else {
				ftmp += 0x80;
			}
		}
		if (!bzm_pi) {
			zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM);
			if (zpi != PI_EN) {
				bzm_pi = 1;
				g_af_ctl.zm_steps &= 0xFF00;
				an41908a_cmd_write(AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
			} else {
				ztmp += 0x80;
			}
		}
		if (ftmp >= fcnt || ztmp >= zcnt) {
			DRV_AF_ERR("Find PI failed, ftmp: %d, fcnt: %d, ztmp: %d, zcnt: %d\n", ftmp, fcnt, ztmp, zcnt);
			return -1;
		}
		if (g_af_ctl.af_exit == 1) {
			DRV_AF_DBG("find zf_pi failed or try to exit the set_zf_pi %d\n", g_af_ctl.af_exit);
			return -1;
		}
		an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		af_msleep(20);
		if (!bfs_pi) {
			an41908a_wait_fs_stop();
		}
		if (!bzm_pi) {
			an41908a_wait_zm_stop();
		}
	}

	// Slow run to High level side
	ftmp = 0;
	ztmp = 0;
	bzm_pi = 0;
	bfs_pi = 0;
	fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS);
	zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM);
	if (PI_DN == g_af_info.fpi_status) {
		g_af_ctl.fs_steps = (fpi == PI_DN) ? (g_af_ctl.fs_steps | 0x0100) : (g_af_ctl.fs_steps & 0xFE00);
	} else {
		g_af_ctl.fs_steps = (fpi == PI_DN) ? (g_af_ctl.fs_steps & 0xFE00) : (g_af_ctl.fs_steps | 0x0100);
	}
	if (PI_DN == g_af_info.zpi_status) {
		g_af_ctl.zm_steps = (zpi == PI_DN) ? (g_af_ctl.zm_steps | 0x0100) : (g_af_ctl.zm_steps & 0xFE00);
	} else {
		g_af_ctl.zm_steps = (zpi == PI_DN) ? (g_af_ctl.zm_steps & 0xFE00) : (g_af_ctl.zm_steps | 0x0100);
	}
	g_af_ctl.fs_steps |= PULSE_ON;    // open pi control switch
	g_af_ctl.zm_steps |= PULSE_ON;    // open pi control switch
	g_af_ctl.fs_steps |= speed;
	g_af_ctl.zm_steps |= speed;
	an41908a_cmd_write(AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_cmd_write(AN41908A_ZOOM_STEPS_REG,  g_af_ctl.zm_steps);
	while (!bzm_pi || !bfs_pi) {
		g_af_ctl.zm_irq_dn = 0;
		g_af_ctl.fs_irq_dn = 0;
		if (!bfs_pi) {
			fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS);
			if (fpi != PI_DN) {
				bfs_pi = 1;
				g_af_ctl.fs_steps &= 0xFF00;
				g_af_ctl.fs_steps &= ~PULSE_ON; // close pi control switch
				an41908a_cmd_write(AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
			} else {
				ftmp += speed;
			}
		}
		if (!bzm_pi) {
			zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM);
			if (zpi != PI_DN) {
				bzm_pi = 1;
				g_af_ctl.zm_steps &= 0xFF00;
				g_af_ctl.zm_steps &= ~PULSE_ON; // close pi control switch
				an41908a_cmd_write(AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
			} else {
				ztmp += speed;
			}
		}
		if (ftmp >= fcnt || ztmp >= zcnt) {
			DRV_AF_ERR("Find PI failed, ftmp: %d, fcnt: %d, ztmp: %d, zcnt: %d\n", ftmp, fcnt, ztmp, zcnt);
			return -1;
		}
		if (g_af_ctl.af_exit == 1) {
			DRV_AF_DBG("find zf_pi failed or try to exit the set_zf_pi %d\n", g_af_ctl.af_exit);
			return -1;
		}
		an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		af_msleep(10);
		if (!bfs_pi) {
			an41908a_wait_fs_stop();
		}
		if (!bzm_pi) {
			an41908a_wait_zm_stop();
		}
	}

#if 0
	fpi_init = fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS); //Goli.milesight modify
	zpi_init = zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM); //Goli.milesight modify
	// Write zoom and focus run register.
	g_af_ctl.fs_steps &= 0xFE00;
	g_af_ctl.zm_steps &= 0xFE00;
	g_af_ctl.fs_steps |= speed;
	g_af_ctl.zm_steps |= speed;

	if (PI_DN == g_af_info.fpi_status)  //special for LENS DF019N
	{
		if (fpi == PI_DN) // near point, move to the far point
		{
			g_af_ctl.fs_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	else
	{
		if (fpi == PI_EN) // near point, move to the far point
		{
			g_af_ctl.fs_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	
	if (PI_DN == g_af_info.zpi_status)  //special for LENS DF019N
	{
		if (zpi == PI_DN) // near point, move to the far point
		{
			g_af_ctl.zm_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}
	else
	{
		if (zpi == PI_EN) // near point, move to the far point
		{
			g_af_ctl.zm_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
		}
	}

	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG,  g_af_ctl.zm_steps);
	
	g_af_ctl.af_exit = 0;

	while (!bzm_pi || !bfs_pi)
	{
	   
		g_af_ctl.zm_irq_dn = 0;
		g_af_ctl.fs_irq_dn = 0;
		
		// output a VD_FZ pluse
		an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		
		// wait for zoom/focus opration finish.
		if(!bfs_pi)
		{   
			an41908a_wait_fs_stop();
			
			// check PI state
			fpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_FS); //Goli.milesight modify
			// Focus find PI point.
			if(fpi != fpi_init)	
			{
				bfs_pi = 1;
				//2  stop focus
				g_af_ctl.fs_steps &= 0xFF00;
				g_af_ctl.fs_steps &= ~PULSE_ON; // close pi control switch
				an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
			}
		}
		
		if(!bzm_pi)
		{
			an41908a_wait_zm_stop();

			// check PI state
			zpi = hisi_gpio_read(g_stGpio.GIO_AF_PI_ZM); //Goli.milesight modify
			// Focus find PI point.
			if(zpi != zpi_init)	
			{
				bzm_pi = 1;
				//2  stop zoom
				g_af_ctl.zm_steps &= 0xFF00;
				g_af_ctl.zm_steps &= ~PULSE_ON; // close pi control switch
				an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
			}		
		}
		
		if(test_time++ %200 == 0)
		{
			DRV_AF_DBG("fpi_init = %d, fpi = %d \n", fpi_init, fpi);
			DRV_AF_DBG("zpi_init = %d, zpi = %d \n", zpi_init, zpi);
		}
		if(g_af_ctl.af_exit == 1)
		{
			DRV_AF_DBG("find zf_pi failed or try to exit the set_zf_pi %d\n", g_af_ctl.af_exit);
			break;
		}
	}

	//output a VD_FZ pluse
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
#endif	
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_cfg.zm_idx = 0;
	g_af_ctl.af_cfg.fs_idx = 0;
	g_af_ctl.fs_busy = 0;
	g_af_ctl.zm_busy = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);
	hisi_gpio_write(g_stGpio.GIO_LENS_PI_POWER_EN, LENS_LIGHT_OFF); //open lens optocoupler
	
	return 0;
}

static int an41908a_zf_sync(u16 zm_pps, s32 zm_dist, u16 fs_pps, s32 fs_dist)
{
	u8 zm_in = (zm_dist > 0) ? 1 : 0;
	u8 fs_near = (fs_dist > 0) ? 1 : 0;
	unsigned long flags = 0;
	
	an41908_up = 0;  //only for double zoom

	// Set af is running
	spin_lock_irqsave(&af_spinlock, flags);
	g_af_ctl.af_mode = AF_MODE_BOTH;
	g_af_ctl.fs_busy = 1;
	g_af_ctl.zm_busy = 1;
	g_af_ctl.zm_irq_num = 0;
	g_af_ctl.fs_irq_num = 0;
	spin_unlock_irqrestore(&af_spinlock, flags);

	g_af_ctl.zm_idx = zm_dist;
	g_af_ctl.fs_idx = fs_dist;
	g_af_ctl.dist_zm = AF_ABS(zm_dist) * DISTANCE_TO_MICRO;  //8-step -> 256-step;
	g_af_ctl.dist_fs = AF_ABS(fs_dist) * (DISTANCE_TO_MICRO/g_af_info.STEP_MICRO);  //8-step -> 256-step;

	// Recaculate PPS and step.
	g_vd_PSUMxx_zm = VD_PSUMXX(zm_pps);
	g_vd_INTCTxx_zm = VD_INTCTXX(g_vd_PSUMxx_zm);
	g_vd_PSUMxx_fs = VD_PSUMXX(fs_pps);
	g_vd_INTCTxx_fs = VD_INTCTXX(g_vd_PSUMxx_fs);
	
	an41908a_cmd_read( AN41908A_ZOOM_STEPS_REG, (u16*)&g_af_ctl.zm_steps);
	an41908a_cmd_read( AN41908A_FOCUS_STEPS_REG, (u16*)&g_af_ctl.fs_steps);

	// caculate INTCTxx and PUSMxx
	g_af_ctl.vd_num_zm = g_af_ctl.dist_zm/(g_vd_PSUMxx_zm * PSUM_TO_STEP);
	g_af_ctl.vd_num_fs = g_af_ctl.dist_fs/(g_vd_PSUMxx_fs * PSUM_TO_STEP);
	g_af_ctl.vd_PSUM_zm = g_vd_PSUMxx_zm;
	g_af_ctl.vd_PSUM_fs = g_vd_PSUMxx_fs;

	// set pps
	an41908a_cmd_write(AN41908A_ZOOM_PPS_REG, (u16)g_vd_INTCTxx_zm);
	an41908a_cmd_write(AN41908A_FOCUS_PPS_REG, (u16)g_vd_INTCTxx_fs);
	// set step
	g_af_ctl.zm_steps &= 0xFE00;
	g_af_ctl.fs_steps &= 0xFE00;
	if(g_af_ctl.vd_num_zm && g_af_ctl.vd_num_fs)
	{
		g_af_ctl.zm_steps |= g_af_ctl.vd_PSUM_zm;
		g_af_ctl.fs_steps |= g_af_ctl.vd_PSUM_fs;
	}
	else if(g_af_ctl.vd_num_zm && !g_af_ctl.vd_num_fs)
	{
		g_af_ctl.zm_steps |= g_af_ctl.vd_PSUM_zm;
		g_af_ctl.fs_steps |= (g_af_ctl.dist_fs%(g_af_ctl.vd_PSUM_fs * PSUM_TO_STEP))/PSUM_TO_STEP;
	}
	else if((!g_af_ctl.vd_num_zm) && g_af_ctl.vd_num_fs)
	{
		g_af_ctl.zm_steps |= (g_af_ctl.dist_zm%(g_af_ctl.vd_PSUM_zm * PSUM_TO_STEP))/PSUM_TO_STEP;
		g_af_ctl.fs_steps |= g_af_ctl.vd_PSUM_fs;
	}
	else
	{
		g_af_ctl.zm_steps |= (g_af_ctl.dist_zm%(g_af_ctl.vd_PSUM_zm * PSUM_TO_STEP))/PSUM_TO_STEP;
		g_af_ctl.fs_steps |= (g_af_ctl.dist_fs%(g_af_ctl.vd_PSUM_fs * PSUM_TO_STEP))/PSUM_TO_STEP;
	}
	if(!zm_in)
		g_af_ctl.zm_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 
	if(!fs_near)
		g_af_ctl.fs_steps |= 0x0100;	 //Add directiion bit (CCWCWXX) 	

	an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, g_af_ctl.zm_steps);
	an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, g_af_ctl.fs_steps);
	// Clean irq flag,  output a AF_VD_FZ pluse to start af.
	g_af_ctl.zm_irq_dn = 0;
	g_af_ctl.fs_irq_dn = 0;
	an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);

	DRV_AF_DBG("%s: zm_pps: %d, dist_zm: %d, PSUMxx_zm: %d, steps: %d, "
		"INTCTxx_zm: %d, vd_num_zm: %d , zm_irq_num: %d\n", 
		__func__, zm_pps, g_af_ctl.dist_zm/DISTANCE_TO_MICRO, g_af_ctl.zm_steps, 
		g_af_ctl.vd_PSUM_zm, g_vd_INTCTxx_zm, 
		g_af_ctl.dist_zm/(g_af_ctl.vd_PSUM_zm * PSUM_TO_STEP), 
		g_af_ctl.zm_irq_num);
	DRV_AF_DBG("%s: fs_pps: %d, dist_fs: %d, PSUMxx_fs: %d, steps: %d, "
		"INTCTxx_fs: %d, vd_num_fs: %d , fs_irq_num: %d\n", 
		__func__, fs_pps, g_af_ctl.dist_fs/DISTANCE_TO_MICRO, g_af_ctl.fs_steps,
		g_af_ctl.vd_PSUM_fs, g_vd_INTCTxx_fs, 
		g_af_ctl.dist_fs/(g_af_ctl.vd_PSUM_fs * PSUM_TO_STEP), 
		g_af_ctl.fs_irq_num);
	return 0;
}


static int an41908a_zf_interrupt_callback(drv_af_t *af_info)
{
	unsigned long flags = 0;
	
	if(((af_info->vd_num_zm >= 0) && (!af_info->zm_irq_dn)) ||
	 	((af_info->vd_num_fs >= 0) && (!af_info->fs_irq_dn)))
	{   
		return 0;
	}


	if(af_info->vd_num_zm >= 0 || af_info->vd_num_fs >= 0)
	{
		if(af_info->vd_num_zm >= 0)
		{
			af_info->vd_num_zm--;
			if(af_info->vd_num_zm == 0)
			{ 
				//2 Run last steps
				af_info->zm_steps &= 0xFF00;
				af_info->zm_steps |= (af_info->dist_zm%(af_info->vd_PSUM_zm * PSUM_TO_STEP))/PSUM_TO_STEP;
				if((af_info->zm_steps & 0x00ff) == 0) // Stop zoom
				{
					af_info->vd_num_zm--;
				}
				an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, af_info->zm_steps);
			}
			else if(af_info->vd_num_zm < 0)
			{
				//2 Stop zoom
				g_af_ctl.zm_steps &= 0xFF00;
				an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, af_info->zm_steps);
			}
		}

		if(af_info->vd_num_fs >= 0)
		{
			af_info->vd_num_fs--;
			if(af_info->vd_num_fs == 0)
			{
				//2 Run last steps
				af_info->fs_steps &= 0xFF00;
				af_info->fs_steps |= (af_info->dist_fs%(af_info->vd_PSUM_fs * PSUM_TO_STEP))/PSUM_TO_STEP;
				if((af_info->fs_steps & 0x00ff) == 0) // Stop zoom
				{
					af_info->vd_num_fs--;
				}
				an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, af_info->fs_steps);
			}
			else if(af_info->vd_num_fs < 0)
			{
				//2 Stop focus
				af_info->fs_steps &= 0xFF00;
				an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, af_info->fs_steps); 
			}
		}

		// Clean irq flag and output a AF_VD_FZ pluse to start next pulse.
		if(af_info->vd_num_zm >= 0 || af_info->vd_num_fs >= 0)
		{

			af_info->zm_irq_dn = 0;
			af_info->fs_irq_dn = 0;
			an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
		}
		else
		{
			af_info->af_cfg.zm_idx += af_info->zm_idx;
			af_info->af_cfg.fs_idx += af_info->fs_idx;
			

//			if(an41908a_cache_run() == -1)
			{
				spin_lock_irqsave(&af_spinlock, flags);
				af_info->af_mode = AF_MODE_NONE;	
				af_info->fs_busy = 0;
				af_info->zm_busy = 0;
				spin_unlock_irqrestore(&af_spinlock, flags);			
			}

			spin_lock_irqsave(&af_spinlock, flags);
			an41908_up = 1;
			if(drv8833_up&&an41908_up)
		    { 
				up(&af_sem);
		    }
			spin_unlock_irqrestore(&af_spinlock, flags);
		}
	}
	return 0;
}

static irqreturn_t an41908a_busy_irq(int irq, void *dev_id)
{
	af_iq_t *irq_info = (af_iq_t *)dev_id;
	unsigned long flags = 0;
	if((irq_info == NULL) 
		|| (irq_info->irq_info_zm->flag != 'z' 
			&& irq_info->irq_info_fs->flag != 'f'))
	{
		DRV_AF_ERR("%s: unknow irq! \n", __func__);
		up(&af_sem);
		return IRQ_HANDLED;
	}

	// check interrupt flag
//	if (an41908a_CheckState(irq_info->irq_info_zm))
	if (irq_info->irq_info_zm->irq_num == irq) //goli.milesight modify 2020.02.28
	{
		irq_info->af_info->zm_irq_dn = 1;
		irq_info->af_info->zm_irq_num++;
		DRV_AF_DBG("AF_MODE_ZOOM=%d\n",irq_info->af_info->zm_irq_num);
		// clean interrupt flag
		an41908a_ClrIntState(irq_info->irq_info_zm);
	}
//	if (an41908a_CheckState(irq_info->irq_info_fs))
	if (irq_info->irq_info_fs->irq_num == irq) //goli.milesight modify 2020.02.28
	{
		irq_info->af_info->fs_irq_dn = 1;
		irq_info->af_info->fs_irq_num++;
		DRV_AF_DBG("AF_MODE_FOCUS=%d\n",irq_info->af_info->fs_irq_num);

		// clean interrupt flag
		an41908a_ClrIntState(irq_info->irq_info_fs);
	}

		
	switch(irq_info->af_info->af_mode)
	{
	default:
	case AF_MODE_NONE:
		break;
	case AF_MODE_ZOOM:
		if(irq_info->af_info->zm_irq_dn)
		{
			drv_af_t *af_info = irq_info->af_info;
			if(af_info->vd_num_zm >= 0)
			{
				af_info->vd_num_zm--;
				if(af_info->vd_num_zm == 0)
				{
					//2 Run last steps
					af_info->zm_steps &= 0xFF00;
					af_info->zm_steps |= (af_info->dist_zm%(af_info->vd_PSUM_zm * PSUM_TO_STEP))/PSUM_TO_STEP;
					if((af_info->zm_steps & 0x00ff) == 0) // Stop zoom
					{
						af_info->vd_num_zm--;
					}
					an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, af_info->zm_steps);
				}
				else if(af_info->vd_num_zm < 0)
				{
					//2 Stop zoom
					g_af_ctl.zm_steps &= 0xFF00;
					an41908a_cmd_write( AN41908A_ZOOM_STEPS_REG, af_info->zm_steps);
				}
			}
			if(af_info->vd_num_zm >= 0)
			{
				// Clean irq flag and output a AF_VD_FZ pluse to start next pulse.
				af_info->zm_irq_dn = 0;
				an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
			}
			else
			{
				af_info->af_cfg.zm_idx += af_info->zm_idx;
//				if(an41908a_cache_run() == -1)
				{
					spin_lock_irqsave(&af_spinlock, flags);
					af_info->af_mode = AF_MODE_NONE;	
					af_info->fs_busy = 0;
					af_info->zm_busy = 0;
					spin_unlock_irqrestore(&af_spinlock, flags);
				}

				spin_lock_irqsave(&af_spinlock, flags);
				an41908_up = 1;
				if(drv8833_up&&an41908_up)
			    {
					up(&af_sem);
			    }
				spin_unlock_irqrestore(&af_spinlock, flags);
			}
		}
		break;

	case AF_MODE_FOCUS:
		if(irq_info->af_info->fs_irq_dn)
		{
			drv_af_t *af_info = irq_info->af_info;
			if(af_info->vd_num_fs >= 0)
			{
				af_info->vd_num_fs--;
				if(af_info->vd_num_fs == 0)
				{
					//2 Run last steps
					af_info->fs_steps &= 0xFF00;
					af_info->fs_steps |= (af_info->dist_fs%(af_info->vd_PSUM_fs * PSUM_TO_STEP))/PSUM_TO_STEP;
					if((af_info->fs_steps & 0x00ff) == 0) // Stop zoom
					{
						af_info->vd_num_fs--;
					}
					an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, af_info->fs_steps);
				}
				else if(af_info->vd_num_fs < 0)
				{
					//2 Stop focus
					af_info->fs_steps &= 0xFF00;
					an41908a_cmd_write( AN41908A_FOCUS_STEPS_REG, af_info->fs_steps);
				}
			}
			if(af_info->vd_num_fs >= 0)
			{
				// Clean irq flag and output a AF_VD_FZ pluse to start next pulse.
				af_info->fs_irq_dn = 0;
				an41908a_out_vd(g_stGpio.GIO_AF_VD_FZ);
			}
			else
			{
				af_info->af_cfg.fs_idx += af_info->fs_idx;
//				if(an41908a_cache_run() == -1)
				{
					spin_lock_irqsave(&af_spinlock, flags);
					af_info->af_mode = AF_MODE_NONE;
					af_info->fs_busy = 0;
					af_info->zm_busy = 0;
					spin_unlock_irqrestore(&af_spinlock, flags);
				}

				spin_lock_irqsave(&af_spinlock, flags);
				an41908_up = 1;
				if(drv8833_up&&an41908_up)
			    {	
					up(&af_sem);
			    }
				spin_unlock_irqrestore(&af_spinlock, flags);
			}
		}
		break;
		
	case AF_MODE_BOTH:
		an41908a_zf_interrupt_callback(irq_info->af_info);
		break;
	}
	return IRQ_HANDLED;
}

static int spi_gpio_mode_init(void)
{
	reg_iocfg_base = (void*)ioremap(0x114F0000, 0x10000);
    if (NULL == reg_iocfg_base)
    {
		iounmap(reg_iocfg_base);
        reg_iocfg_base = 0;
		return -1;
    }
	
	AF_WRITEL(reg_iocfg_base+0x50, 0x400); 
	AF_WRITEL(reg_iocfg_base+0x54, 0x400);
	AF_WRITEL(reg_iocfg_base+0x58, 0x400);
	AF_WRITEL(reg_iocfg_base+0x5C, 0x400);
	
	hisi_gpio_write(GPIO_SPI2_CS0, 0);
	hisi_gpio_write(GPIO_SPI2_CLK, 1);
	hisi_gpio_write(GPIO_SPI2_SDO, 0);
	hisi_gpio_mode(GPIO_SP2_SDI, GIO_NOIRQ_INPUT);
	
	return 0;
}

static int an41908a_init_reg()
{
	memset(&g_af_ctl, 0, sizeof(drv_af_t));
	g_af_ctl.af_mode = AF_MODE_NONE;
	g_af_ctl.iris_duty = 10;
	g_vd_PSUMxx_zm = VD_PSUMXX(g_af_info.zm_max_pps);
	g_vd_INTCTxx_zm = VD_INTCTXX(g_vd_PSUMxx_zm);
	g_vd_PSUMxx_fs = VD_PSUMXX(g_af_info.fs_max_pps);
	g_vd_INTCTxx_fs = VD_INTCTXX(g_vd_PSUMxx_fs);
	
	if (SPI_GPIO == g_af_info.spi_mode)
	{
		spi_gpio_mode_init();
	}
	
#if 1	
//	hisi_gpio_mode(g_stGpio.GIO_AF_PI_FS, GIO_NOIRQ_INPUT);
//	hisi_gpio_mode(g_stGpio.GIO_AF_PI_ZM, GIO_NOIRQ_INPUT);
	hisi_gpio_mode(g_stGpio.GIO_AF_PLS1_FS, GIO_IRQ_INPUT);
	hisi_gpio_mode(g_stGpio.GIO_AF_PLS2_ZM, GIO_IRQ_INPUT);
#endif
	
	an41908a_init_irq();
	
	/*reset an41908*/
	hisi_gpio_write(g_stGpio.GIO_AF_RESET, 0);
	af_msleep(50);
	hisi_gpio_write(g_stGpio.GIO_AF_RESET, 1);
	af_msleep(200);
	
	an41908a_IrisInit();
	an41908a_FZInit();
	an41908a_set_aperture(0);

	
	return 0;
}

int bu24036mwv_piris_init_gpio(const lens_drv_gpio_t *pstDrvGpio)
{
	memcpy(&g_stGpio, pstDrvGpio, sizeof(lens_drv_gpio_t));
	
	return 0;
}
EXPORT_SYMBOL(bu24036mwv_piris_init_gpio);

int bu24036mwv_init_reg(driver_lens_type_e driver_type)
{
	memset(&g_af_ctl, 0, sizeof(drv_af_t));
	g_af_ctl.af_mode = AF_MODE_NONE;
	g_af_ctl.iris_duty = 10;
	
	hisi_gpio_mode(g_stGpio.GIO_BU24_B2, GIO_NOIRQ_INPUT);
	hisi_gpio_mode(g_stGpio.GIO_BU24_A2, GIO_NOIRQ_INPUT);
	hisi_gpio_mode(g_stGpio.GIO_BU24_B1, GIO_NOIRQ_INPUT);
	hisi_gpio_mode(g_stGpio.GIO_BU24_A1, GIO_NOIRQ_INPUT);
	
	if (SPI_GPIO == g_af_info.spi_mode)
	{
		spi_gpio_mode_init();
	}
	
	if (g_af_info.driver_lens_type == DRIVER_LENS_ABF)
	{
		abf_init_irq(); //for ABF_key, young.milesight add 18.9.12		
	}		
	bu24036mwv_zf_init(driver_type);
	bu24036mwv_iris_init();

	return 0;
}
EXPORT_SYMBOL(bu24036mwv_init_reg);

static inline int an41908a_check_busy()
{
	int ret;
	unsigned long flags = 0;
	
//	printk("in check_busy\n");
	spin_lock_irqsave(&af_spinlock, flags);
	ret = (g_af_ctl.zm_busy || g_af_ctl.fs_busy);
	spin_unlock_irqrestore(&af_spinlock, flags);
//	printk("exit check_busy\n");
	return ret;
}

static inline int bu24036mwv_check_busy()
{
	int ret;
	unsigned long flags = 0;
	
	spin_lock_irqsave(&af_spinlock, flags);
	ret = (g_af_ctl.zm_busy || g_af_ctl.fs_busy);
	spin_unlock_irqrestore(&af_spinlock, flags);
	return ret;
}

static inline int bu24036mwv_check_run(void)
{
	int ret;
	unsigned long flags = 0;
	
	spin_lock_irqsave(&af_spinlock, flags);
	ret = (g_af_ctl.zm_busy || g_af_ctl.fs_busy);
	ret |= hisi_gpio_read(g_stGpio.GIO_BU24_B1); //Goli.milesight modify
	ret |= hisi_gpio_read(g_stGpio.GIO_BU24_B2); //Goli.milesight modify
	spin_unlock_irqrestore(&af_spinlock, flags);
	return ret;
}


static int an41908a_run_cmd(int cmd, void* args)
{
	if (g_af_ctl.af_exit)
		return AF_STATUS_SOK;
	
//	DRV_AF_DBG("an41908a_run_cmd=%d\n", cmd);
	switch(cmd)
	{
	case AF_SET_ZF_PI:
		DRV_AF_DBG("AF_SET_ZF_PI\n");
		if(an41908a_check_busy())
		{
			DRV_AF_ERR("af is busy now!\n");
			return -EBUSY;
		}
		if (down_interruptible(&af_sem))
			return - ERESTARTSYS;
		if(g_af_info.lensMode == LENS_MODE_AF)
		{
			an41908a_set_zf_pi(1, 1,4);
			printk("third zf pi ok by 4 step\n");
		}
		else if(g_af_info.lensMode == LENS_MODE_NM1)
		{
			g_af_dist.fs_dist = g_af_info.fs_far_bd - g_af_info.fs_near_bd;
			g_af_dist.zm_dist = g_af_info.zm_out_bd - g_af_info.zm_in_bd;

			g_af_dist.fs_dist -= 64;
			g_af_dist.zm_dist += 16;
			g_af_dist.fs_pps = g_af_info.fs_max_pps;
			g_af_dist.zm_pps = g_af_info.zm_max_pps;
			an41908a_zf_sync(800, g_af_dist.zm_dist,800,-g_af_dist.fs_dist);
		}


		up(&af_sem);
		break;
		
	case AF_SET_ZM_PI:
		DRV_AF_DBG("AF_SET_ZM_PI\n");
		if(an41908a_check_busy())
		{
			DRV_AF_ERR("af is busy now!\n");
			return -EBUSY;
		}		
		if (down_interruptible(&af_sem))
			return - ERESTARTSYS;
		an41908a_set_zm_pi(1, 4);
		printk("third zm pi ok by 1   step\n");

		up(&af_sem);
		break;
	case AF_SET_FS_PI:
		DRV_AF_DBG("AF_SET_FS_PI\n");
		if(an41908a_check_busy())
		{
			DRV_AF_ERR("af is busy now!\n");
			return -EBUSY;
		}
		if (down_interruptible(&af_sem))
			return - ERESTARTSYS;
		an41908a_set_fs_pi(1, 4);
		printk("third fs pi ok by 1   step\n");
		up(&af_sem);
		break;

	case AF_RESET:
		DRV_AF_DBG("AF_RESET\n");
		an41908a_init_reg();
		break;
		
	case IRCUT_RESET:
		DRV_AF_DBG("IRCUT_RESET\n");	
		if(g_af_info.driver_lens_type == DRIVER_LENS_SPD3_0)
			ircut_origin_set();
		else if (g_af_info.driver_lens_type == DRIVER_LENS_SPD2_0)
			register_ircut_cb();
		break;
	case IRCUT_SET_PI:
		DRV_AF_DBG("IRCUT_SET_PI\n");
		if (down_interruptible(&af_sem))
		{
			return - ERESTARTSYS;
		}
		set_ircut_pi();
		up(&af_sem);
		break;
	case IRCUT_RUN:
		DRV_AF_DBG("IRCUT_RUN\n");
		if (down_interruptible(&af_sem))
			return - ERESTARTSYS;
		if(copy_from_user((af_dist_t*)&g_af_dist, 
				(af_dist_t*)args, sizeof(af_dist_t)) != 0 )   
		{
			DRV_AF_ERR("copy_from_user\n");
			up(&af_sem);
			return -EFAULT; 
		}

		if((g_af_dist.zm_dist != 0) || (g_af_dist.fs_dist != 0))
		{
			if(g_af_info.driver_lens_type == DRIVER_LENS_SPD3_0)
				run_fourLines_ircut(g_af_dist.fs_dist);
			else if (g_af_info.driver_lens_type == DRIVER_LENS_SPD2_0)
				run_twoLines_ircut(g_af_dist.fs_dist);
		}
		
		up(&af_sem);
		DRV_AF_DBG("zm_idx = %d, fs_idx = %d \n", g_af_dist.zm_dist, g_af_dist.fs_dist);
		break;	
	case AF_RUN:
		DRV_AF_DBG("AF_RUN\n");
		if (down_interruptible(&af_sem))
		{
			return - ERESTARTSYS;
		}
			
		if(copy_from_user((af_dist_t*)&g_af_dist, 
				(af_dist_t*)args, sizeof(af_dist_t)) != 0 )   
		{
			DRV_AF_ERR("copy_from_user\n");
			up(&af_sem);
			return -EFAULT; 
		}
		//check parameters
		if(g_af_info.lensMode == LENS_MODE_NM1)
		{
			g_af_dist.zm_dist = -g_af_dist.zm_dist;
			g_af_dist.fs_dist = -g_af_dist.fs_dist;
		}

		if((g_af_dist.zm_dist != 0) && (g_af_dist.fs_dist != 0))
		{	
			DRV_AF_DBG("an41908a_zf_sync, zm_pps(%d),zm_dist(%d),fs_pps(%d),fs_idx(%d)\n",
				g_af_dist.zm_pps, g_af_dist.zm_dist,
				g_af_dist.fs_pps, g_af_dist.fs_dist);
			an41908a_zf_sync( g_af_dist.zm_pps, g_af_dist.zm_dist,
						g_af_dist.fs_pps, g_af_dist.fs_dist);
		}
		else if((g_af_dist.zm_dist != 0) && (g_af_dist.fs_dist == 0))
		{	
			DRV_AF_DBG("an41908a_zoom_run, zm_pps(%d),zm_dist(%d)\n",
				g_af_dist.zm_pps, g_af_dist.zm_dist);
			an41908a_zoom_run(g_af_dist.zm_pps, g_af_dist.zm_dist);
		}
		else if((g_af_dist.zm_dist == 0) && (g_af_dist.fs_dist != 0))
		{	
			DRV_AF_DBG("an41908a_focus_run, fs_pps(%d),fs_idx(%d)\n",
				g_af_dist.fs_pps, g_af_dist.fs_dist);
			an41908a_focus_run(g_af_dist.fs_pps, g_af_dist.fs_dist);

		}
		else
		{
			up(&af_sem);
			return 0;
		}

		DRV_AF_DBG("zm_idx = %d, fs_idx = %d \n", g_af_ctl.af_cfg.zm_idx, g_af_ctl.af_cfg.fs_idx);
		break;
		
	case AF_CHECK_BUSY:
//		DRV_AF_DBG("AF_CHECK_BUSY(%d)\n", an41908a_check_busy());
		{
			u32 af_state;

			af_state = an41908a_check_busy();
			put_user(af_state, (int*)args);
		}	
		
		break;

	case AF_SET_IRIS:
		DRV_AF_DBG("AF_SET_IRIS\n");
		{
			get_user(g_af_ctl.iris_duty, (u16*)args);
			g_af_ctl.iris_duty = g_af_info.iris_open_bd - g_af_ctl.iris_duty;
			an41908a_set_aperture(g_af_ctl.iris_duty);
		}
		break;

	case AF_SET_IRCUT:
		DRV_AF_DBG("AF_SET_IRCUT\n");
		{
			u8 ircut_status = 0;

			get_user(ircut_status, (u8*)args);
			an41908a_set_ircut(ircut_status);
		}
		break;
		
	case AF_STOP_ZM:
		DRV_AF_DBG("AF_STOP_ZM\n");
		{
			if (down_interruptible(&af_sem))
				return - ERESTARTSYS;
			an41908a_zoom_stop();
			up(&af_sem);
		}
		break;

	case AF_STOP_FS:
		DRV_AF_DBG("AF_STOP_FS\n");
		{
			if (down_interruptible(&af_sem))
				return - ERESTARTSYS;
			an41908a_focus_stop();
			up(&af_sem);
		}
		break;
	case AF_EXIT:
		DRV_AF_DBG("AF_EXIT\n");
		g_af_ctl.af_exit = 1;
		break;
	default://cmd error
		DRV_AF_ERR("No such af command!!\n");
		return -EINVAL;
	}

	return AF_STATUS_SOK;
}

static int abf_optocoupler_init(void)
{
	int opt_flag; 

	DRV_ABF_DBG("abf_optocoupler_init\n");
	opt_flag = hisi_gpio_read(g_stGpio.GIO_BOX_PI);
	msleep(50);
	opt_flag = hisi_gpio_read(g_stGpio.GIO_BOX_PI);
	while(bu24036mwv_check_busy())
	{
		af_usleep(500);
	}
	g_af_dist.fs_pps = g_af_info.fs_max_pps;
	g_af_dist.zm_pps = g_af_info.zm_max_pps;
	DRV_ABF_DBG("opt_flag=%d\n", opt_flag);
	if(opt_flag)
	{
		while(opt_flag)
		{
			bu24036mwv_zf_run( g_af_dist.zm_pps, 0, g_af_dist.fs_pps, -1);
			opt_flag = hisi_gpio_read(g_stGpio.GIO_BOX_PI);
			DRV_ABF_DBG("111111111opt_flag=%d\n", opt_flag);
			if(g_af_ctl.af_exit == 1)
			{
				DRV_AF_DBG("find abf_pi failed or try to exit the abf_pi %d\n", g_af_ctl.af_exit);
				break;
			}
		}
	}
	else
	{
		while(!opt_flag)
		{
			bu24036mwv_zf_run( g_af_dist.zm_pps, 0, g_af_dist.fs_pps, 1);
			opt_flag = hisi_gpio_read(g_stGpio.GIO_BOX_PI);
			DRV_ABF_DBG("22222222opt_flag=%d\n", opt_flag);
			if(g_af_ctl.af_exit == 1)
			{
				DRV_AF_DBG("find abf_pi failed or try to exit the abf_pi %d\n", g_af_ctl.af_exit);
				break;
			}
		}
	}
	
	return 0;
}

static int bu24036mwv_set_zf_pi(void)
{
	if(g_af_info.b_fs_init)
		g_af_dist.fs_dist = g_af_info.fs_far_bd - g_af_info.fs_pos;
	else
		g_af_dist.fs_dist = g_af_info.fs_far_bd - g_af_info.fs_near_bd;
	if(g_af_info.b_zm_init)
		g_af_dist.zm_dist = g_af_info.zm_out_bd - g_af_info.zm_pos;
	else	
		g_af_dist.zm_dist = g_af_info.zm_out_bd - g_af_info.zm_in_bd;
	g_af_dist.fs_dist -= 64;
	g_af_dist.zm_dist += 16;
	g_af_dist.fs_pps = g_af_info.fs_max_pps;
	g_af_dist.zm_pps = g_af_info.zm_max_pps;
	bu24036mwv_zf_run( g_af_dist.zm_pps, g_af_dist.zm_dist,g_af_dist.fs_pps, g_af_dist.fs_dist);
	
	return 0;
}

static int bu24036mwv_run_cmd(int cmd, void* args)
{
	if (g_af_ctl.af_exit)
		return AF_STATUS_SOK;
	
	switch(cmd)
	{
	case AF_SET_ZF_PI:
		DRV_AF_DBG("AF_SET_ZF_PI\n");
		if (g_af_info.driver_lens_type == DRIVER_LENS_ABF)
		{
			abf_optocoupler_init();
			bu24036mwv_zoom_stop();
		}
		else
		{
			if(bu24036mwv_check_busy())
			{
				DRV_AF_ERR("af is busy now!\n");
				return -EBUSY;
			}
			if (down_interruptible(&af_sem))
				return - ERESTARTSYS;
			bu24036mwv_set_zf_pi();
			up(&af_sem);
		}
		break;
		
	case AF_SET_ZM_PI:
		DRV_AF_DBG("AF_SET_ZM_PI\n");
		if(bu24036mwv_check_busy())
		{
			DRV_AF_ERR("af is busy now!\n");
			return -EBUSY;
		}		
		break;
		
	case AF_SET_FS_PI:
		DRV_AF_DBG("AF_SET_FS_PI\n");
		if(bu24036mwv_check_busy())
		{
			DRV_AF_ERR("af is busy now!\n");
			return -EBUSY;
		}		
		break;

	case AF_RESET:
		DRV_AF_DBG("AF_RESET \n");	
		bu24036mwv_init_reg(g_af_info.driver_lens_type);
		DRV_AF_DBG("AF_RESET over\n");
		break;
		
	case AF_RUN:
		DRV_AF_DBG("AF_RUN\n");
		
		if (down_interruptible(&af_sem))
			return - ERESTARTSYS;
		if(copy_from_user((af_dist_t*)&g_af_dist, 
				(af_dist_t*)args, sizeof(af_dist_t)) != 0 )   
		{
			DRV_AF_ERR("copy_from_user\n");
			up(&af_sem);
			return -EFAULT; 
		}
		
		//check parameters
		if((g_af_dist.zm_dist != 0) || (g_af_dist.fs_dist != 0))
		{	
			bu24036mwv_zf_run( g_af_dist.zm_pps, g_af_dist.zm_dist,
								g_af_dist.fs_pps, g_af_dist.fs_dist);
		}
		up(&af_sem);
		DRV_AF_DBG("zm_idx = %d, fs_idx = %d \n", g_af_dist.zm_dist, g_af_dist.fs_dist);
		break;
		
	case AF_CHECK_BUSY:
//		DRV_AF_DBG("AF_CHECK_BUSY(%d)\n", bu24036mwv_check_busy());
		{
			u32 af_state;
			af_state = bu24036mwv_check_run();
			put_user(af_state, (int*)args);
		}	
		break;
		
	case AF_SET_IRIS:
		DRV_AF_DBG("AF_SET_IRIS\n");
		{
			get_user(g_af_ctl.iris_duty, (u16*)args);
			g_af_ctl.iris_duty = g_af_info.iris_open_bd - g_af_ctl.iris_duty;
			bu24036mwv_set_aperture(g_af_ctl.iris_duty);
//			printk("Current iris duty: %d \n", g_af_ctl.iris_duty);
		}
		break;

	case AF_SET_IRCUT:
		DRV_AF_DBG("AF_SET_IRCUT\n");
		{
			u8 ircut_status = 0;

			get_user(ircut_status, (u8*)args);
			bu24036mwv_set_ircut(ircut_status);
		}
		break;
		
	case AF_STOP_ZM:
		DRV_AF_DBG("AF_STOP_ZM\n");
		{
			if (down_interruptible(&af_sem))
				return - ERESTARTSYS;
			bu24036mwv_zoom_stop();
			up(&af_sem);
		}
		break;

	case AF_STOP_FS:
		DRV_AF_DBG("AF_STOP_FS\n");
		{
			if (down_interruptible(&af_sem))
				return - ERESTARTSYS;
			bu24036mwv_focus_stop();
			up(&af_sem);
		}
		break;

	case AF_EXIT:
		DRV_AF_DBG("AF_EXIT\n");
		g_af_ctl.af_exit = 1;
		break;
		
	default://cmd error
		DRV_AF_ERR("No such af command!!\n");
		return -EINVAL;
	}

	return AF_STATUS_SOK;
}


static int double_run_cmd(int cmd, void* args)
{
	if (g_af_ctl.af_exit)
		return AF_STATUS_SOK;
	switch(cmd)
	{
	case AF_SET_ZM_PI:
		if(an41908a_check_busy())
		{
		    DRV_AF_ERR("an41908 busy\n");
			return -EBUSY;
		}
		if(down_interruptible(&af_sem))
			return - ERESTARTSYS;
//		an41908a_set_zf_pi(1, 1,100);
//		printk("first  zf pi ok  by 100 step\n");
//		an41908a_set_zf_pi(1, 1,10);
//		printk("second zf pi ok by 10  step\n");
		an41908a_set_zf_pi(1, 1,4);
		printk("third zf pi ok by 1   step\n");


		up(&af_sem);
		break;
		
	case AF_SET_FS_PI:
		if(drv8833_check_busy())
		{
		    DRV_AF_DBG("drv8833 busy\n");
			return -EBUSY;
		}
		if(down_interruptible(&af_sem))
			return - ERESTARTSYS;
		drv8833_set_pi();
		up(&af_sem);
		break;
		
	case AF_SET_ZF_PI:
		zm2_pitime =0;
		if(an41908a_check_busy()){
			DRV_AF_ERR("an41908 busy\n");
			return -EBUSY;
		}
		if(down_interruptible(&af_sem)){
			return - ERESTARTSYS;
		}
		drv8833_set_pi();
		an41908a_set_zf_pi(1, 1, 4);
		printk("third zf pi ok by 4   step\n");
	    zm1_idx   = 0; 
        zm2_idx   = 0; 
		up(&af_sem);
		break;

		
	case AF_RESET:
	    drv_8833_gpioinit();  
		an41908a_init_reg();
		break;
		
	case AF_RUN:
		if (down_interruptible(&af_sem)){
			return - ERESTARTSYS;
		}
		if(copy_from_user((af_dist_t*)&g_af_dist, (af_dist_t*)args, sizeof(af_dist_t)) != 0 ){
		    up(&af_sem);
			DRV_AF_DBG("copy_from_user1\n");
			return -EFAULT;
		}
        if( (g_af_dist.zm_dist == 0) && (g_af_dist.zm2_dist == 0) && (g_af_dist.fs_dist == 0)){
            up(&af_sem);
		}
		if(g_af_dist.zm2_dist != 0){
			drv8833_run(g_af_dist.zm2_pps, -g_af_dist.zm2_dist);	
		}

		if((g_af_dist.zm_dist != 0) && (g_af_dist.fs_dist != 0)){	
			an41908a_zf_sync( g_af_dist.zm_pps, g_af_dist.zm_dist, g_af_dist.fs_pps, -g_af_dist.fs_dist);
			zm1_idx += g_af_dist.zm_dist;
			fs_idx  += g_af_dist.fs_dist;
		}
		else if((g_af_dist.zm_dist != 0) && (g_af_dist.fs_dist == 0)){	
			an41908a_zoom_run(g_af_dist.zm_pps, g_af_dist.zm_dist);
			zm1_idx += g_af_dist.zm_dist;
		}
		else if((g_af_dist.zm_dist == 0) && (g_af_dist.fs_dist != 0)){	
			an41908a_focus_run(g_af_dist.fs_pps, -g_af_dist.fs_dist);
			fs_idx += g_af_dist.fs_dist;
		}
		break;


		
	case AF_CHECK_BUSY:
		{
			u32 af_state;
			af_state = an41908a_check_busy();
			put_user(af_state, (int*)args);
		}	
		break;
		
	case AF_SET_IRIS:
		{
			get_user(g_af_ctl.iris_duty, (u16*)args);
			g_af_ctl.iris_duty = g_af_info.iris_open_bd - g_af_ctl.iris_duty;
			an41908a_set_aperture(g_af_ctl.iris_duty);
		}
		break;
		
	case AF_SET_IRCUT:
		break;

	case AF_STOP_FS:
		break;	
		
	case AF_STOP_ZM:
		break;	
		
	case IRCUT_RESET:	
		break;
		
	case IRCUT_SET_PI:
		break;
		
	case IRCUT_RUN:
		break;	
		
	case AF_EXIT:
		g_af_ctl.af_exit = 1;
		break;
		
	default:
		return -EINVAL;
	}
	return AF_STATUS_SOK;
}


//af init
static int af_open( struct inode *inode, struct file *file)
{
	DRV_AF_DBG("Open af success!\n");
	
	return AF_STATUS_SOK;
}

static int af_release(struct inode *inode, struct file *file)
{
	DRV_AF_DBG("Release af success!\n");

	return AF_STATUS_SOK;
}

static int af_read(struct file *file, char *buf, size_t count, loff_t *f_pos)
{
	return AF_STATUS_SOK;
}

static long af_ioctl(struct file *filp, unsigned int cmd,  unsigned long args)
{
	cmd = _IOC_NR(cmd);
	
	switch(cmd)
	{
	case AF_INIT_PARAS:
		DRV_AF_DBG("AF_INIT_PARAS\n");
		if(copy_from_user((af_init_params_t*)&g_af_info, (af_init_params_t*)args, sizeof(af_init_params_t)) != 0 )	
		{
			DRV_AF_ERR("copy_from_user\n");
			return -EFAULT;
		}
		memcpy(&g_stGpio, &g_af_info.stDrvGpio, sizeof(lens_drv_gpio_t));
		
		printk("g_af_info.driver_lens_type=%d\n", g_af_info.driver_lens_type);
		
		if (g_af_info.chip_type == CHIP_AN41908)
		{
			printk("an41908 gpio==(%d, %d, %d, %d), (%d, %d, %d, %d)\n", 
				g_stGpio.GIO_LENS_PI_POWER_EN,g_stGpio.GIO_AF_RESET, 
				g_stGpio.GIO_AF_VD_FZ, g_stGpio.GIO_AF_VD_IS, 
				g_stGpio.GIO_AF_PI_ZM, g_stGpio.GIO_AF_PI_FS, 
				g_stGpio.GIO_AF_PLS2_ZM, g_stGpio.GIO_AF_PLS1_FS);
		}
		else if(g_af_info.chip_type == CHIP_BU24036MWV)
		{
			printk("bu24036 gpio==(%d, %d, %d, %d, %d, %d)\n", 
				g_stGpio.GIO_BU24_A1,g_stGpio.GIO_BU24_A2, 
				g_stGpio.GIO_BU24_B1, g_stGpio.GIO_BU24_B2, 
				g_stGpio.GIO_BOX_KEY, g_stGpio.GIO_BOX_PI);
		}
		else if(g_af_info.chip_type == CHIP_DOUBLE)
		{
			printk("an41908 gpio==(%d, %d, %d, %d), (%d, %d, %d, %d)  drv8833 gpio =(%d, %d, %d, %d, %d)\n", 
				g_stGpio.GIO_LENS_PI_POWER_EN,g_stGpio.GIO_AF_RESET, 
				g_stGpio.GIO_AF_VD_FZ, g_stGpio.GIO_AF_VD_IS, 
				g_stGpio.GIO_AF_PI_ZM, g_stGpio.GIO_AF_PI_FS, 
				g_stGpio.GIO_AF_PLS2_ZM, g_stGpio.GIO_AF_PLS1_FS,
				g_stGpio.GIO_8833_A1,g_stGpio.GIO_8833_A2,
				g_stGpio.GIO_8833_B1,g_stGpio.GIO_8833_B2,
				g_stGpio.GIO_8833_PI_FS);
		}
		g_af_ctl.af_cfg.fs_idx = g_af_info.fs_pos;
		g_af_ctl.af_cfg.zm_idx = g_af_info.zm_pos;
		g_af_ctl.af_exit = 0;
		if(g_af_info.chip_type == CHIP_AN41908)
		{
			g_vd_PSUMxx_zm  = VD_PSUMXX(g_af_info.zm_max_pps);
			g_vd_INTCTxx_zm = VD_INTCTXX(g_vd_PSUMxx_zm);
			g_vd_PSUMxx_fs  = VD_PSUMXX(g_af_info.fs_max_pps);
			g_vd_INTCTxx_fs = VD_INTCTXX(g_vd_PSUMxx_fs);
		}
		break;
	default:
		switch(g_af_info.chip_type)
		{
		case CHIP_AN41908:
			return an41908a_run_cmd(cmd, (void*)args);
		case CHIP_BU24036MWV:
			return bu24036mwv_run_cmd(cmd, (void*)args);
		case CHIP_DOUBLE:
			return double_run_cmd(cmd, (void*)args);
		default:
			break;
		}
		break;
	}
	return AF_STATUS_SOK;
}


static struct file_operations af_fops = 
{
	.owner = THIS_MODULE,
	.open = af_open,
	.read = af_read,	
	.release = af_release,
	.unlocked_ioctl = af_ioctl,

};

static void banner(void)
{
	printk(KERN_INFO "DRV_AF module: built on " __DATE__ " at " __TIME__ "\n");
	printk(KERN_INFO "  Reference Linux version %d.%d.%d\n",
		   (LINUX_VERSION_CODE & 0x00ff0000) >> 16,
		   (LINUX_VERSION_CODE & 0x0000ff00) >> 8,
		   (LINUX_VERSION_CODE & 0x000000ff) >> 0
		  );
	printk("\tdrv_af module version %s\n", AF_MOUDLE_VERSION);
	printk(KERN_INFO "\tFile " __FILE__ "\n");
}

static struct miscdevice hiaf_miscdev =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DEVICE_AF_NAME,
	.fops  = &af_fops,
};

static int __init __af_init(void)
{
	int ret = 0;
	
	printk("\n\n===99999===__af_init====\n\n");
	
	banner();
	// Register chrdev
	ret = misc_register(&hiaf_miscdev);
	if( ret < 0 )
	{
		DRV_AF_ERR( "misc_register af failed = %d\n", ret);
		return -ENODEV;
	}
	DRV_AF_DBG(" Register af driver OK!\n");
	
	// Spi init
	ret = af_spi_init();
	if(ret != 0)
	{
		misc_deregister(&hiaf_miscdev);
		DRV_AF_ERR( "af_spi_init af failed = %d\n", ret);
		return ret;
	}
	
	printk(KERN_INFO "AF initialized!\n");
	return AF_STATUS_SOK;
}

static void __exit __af_exit(void)
{	
	an41908a_irq_free();
	af_hrtimer_cancel();

	misc_deregister(&hiaf_miscdev);
	DRV_AF_DBG("Unregistered af success!\n");
	printk("AF uninstalled! \n");
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("EricZheng");
module_init(__af_init);
module_exit(__af_exit);



