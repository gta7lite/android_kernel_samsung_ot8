/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef KPD_HAL_H
#define KPD_HAL_H
#include <mt-plat/sync_write.h>
/* Keypad registers */
#define PMIC_KEY_STATUS
extern void __iomem *kp_base;
extern struct input_dev *kpd_input_dev;

#define KP_STA			(kp_base + 0x0000)
#define KP_MEM1			(kp_base + 0x0004)
#define KP_MEM2			(kp_base + 0x0008)
#define KP_MEM3			(kp_base + 0x000c)
#define KP_MEM4			(kp_base + 0x0010)
#define KP_MEM5			(kp_base + 0x0014)
#define KP_DEBOUNCE		(kp_base + 0x0018)
#define KP_SCAN_TIMING	(kp_base + 0x001C)
#define KP_SEL			(kp_base + 0x0020)
#define KP_EN			(kp_base + 0x0024)

#define KP_COL0_SEL             (1 << 10)
#define KP_COL1_SEL             (1 << 11)
#define KP_COL2_SEL             (1 << 12)

#define KPD_DEBOUNCE_MASK	((1U << 14) - 1)
#define KPD_DOUBLE_KEY_MASK	(1U << 0)

/*function define*/
//void kpd_slide_qwerty_init(void);
//void kpd_ldvt_test_init(void);
void long_press_reboot_function_setting(void);
//void kpd_auto_test_for_factorymode(void);
void kpd_wakeup_src_setting(int enable);
void kpd_get_keymap_state(u16 state[]);
void kpd_set_debounce(u16 val);
void kpd_init_keymap(u32 keymap[]);
void kpd_init_keymap_state(u16 keymap_state[]);
#if defined(PMIC_KEY_STATUS)
extern unsigned int mt6358_upmu_get_pwrkey_deb(void);
extern unsigned int mt6358_upmu_get_homekey_deb(void);
unsigned int kpd_pmic_pwrkey_status_hal(void);
unsigned int kpd_pmic_homekey_status_hal(void);
#endif
void kpd_pmic_rstkey_hal(unsigned long pressed);
void kpd_pmic_pwrkey_hal(unsigned long pressed);
void kpd_pwrkey_handler_hal(unsigned long data);
void mt_eint_register(void);
void kpd_enable(int enable);
void kpd_double_key_enable(int en);

#define KPD_NUM_MEMS	5
#define KPD_MEM5_BITS	8

#define KPD_NUM_KEYS	72	/* 4 * 16 + KPD_MEM5_BITS */
#endif
/*hs14 code for AL6528ADEU-723 by hehaoran5 at 20221021 start*/
#if defined(CONFIG_HQ_PROJECT_O22) || defined(CONFIG_HQ_PROJECT_O8)
#include <mt-plat/mtk_boot_common.h>
extern enum boot_mode_t tp_get_boot_mode(void);
#endif
/*hs14 code for AL6528ADEU-723 by hehaoran5 at 20221021 end*/
