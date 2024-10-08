/*
 * GalaxyCore touchscreen driver
 *
 * Copyright (C) 2021 GalaxyCore Incorporated
 *
 * Copyright (C) 2021 Neo Chen <neo_chen@gcoreinc.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "gcore_drv_common.h"

static int tpd_local_init(void);
static void tpd_suspend(struct device *h);
static void tpd_resume(struct device *h);

static struct tpd_driver_t tpd_device_driver = {
    .tpd_device_name = "gcore",
    .tpd_local_init = tpd_local_init,
    .suspend = tpd_suspend,
    .resume = tpd_resume,
};

static int tpd_local_init(void)
{
    GTP_DEBUG("tpd_local_init start.");

    if (gcore_touch_bus_init()) {
        GTP_ERROR("bus init fail!");
        return -EPERM;
    }

    if (tpd_load_status == 0) {
        GTP_ERROR("add error touch panel driver.");
        gcore_touch_bus_exit();
        return -EPERM;
    }

    GTP_DEBUG("end %s, %d\n", __func__, __LINE__);
    tpd_type_cap = 1;

    return 0;
}

static void tpd_resume(struct device *h)
{
    GTP_DEBUG("TPD resume start...");

#ifdef CONFIG_ENABLE_GESTURE_WAKEUP
    /*hs03s_NM code for SR-AL5625-01-640 by yuli at 20220420 start*/
    if(fn_data.gdev->gesture_wakeup_en) {
        disable_irq_wake(fn_data.gdev->touch_irq);
    }
    /*hs03s_NM code for SR-AL5625-01-640 by yuli at 20220420 end*/
#endif

#ifdef CONFIG_GCORE_AUTO_UPDATE_FW_HOSTDOWNLOAD
    gcore_request_firmware_update_work(NULL);
#endif

    /*hs04 code for DEAL6398A-522 by huangzhongjie at 20220811 start*/
    msleep(300);
    gcore_fw_event_notify(FW_HEADSET_UNPLUG-fn_data.gdev->earphone_status);
    msleep(10);
    gcore_fw_event_notify(FW_CHARGER_UNPLUG-fn_data.gdev->usb_detect_status);
    /*hs04 code for DEAL6398A-522 by huangzhongjie at 20220811 end*/
    fn_data.gdev->ts_stat = TS_NORMAL;

    GTP_DEBUG("tpd resume end.");
}

static void tpd_suspend(struct device *h)
{
    GTP_DEBUG("TPD suspend start...");

#if GCORE_WDT_RECOVERY_ENABLE
    cancel_delayed_work_sync(&fn_data.gdev->wdt_work);
#endif

    cancel_delayed_work_sync(&fn_data.gdev->fwu_work);

#ifdef CONFIG_ENABLE_GESTURE_WAKEUP
    /*hs03s_NM code for SR-AL5625-01-640 by yuli at 20220420 start*/
    if(fn_data.gdev->gesture_wakeup_en) {
        enable_irq_wake(fn_data.gdev->touch_irq);
    }
    /*hs03s_NM code for SR-AL5625-01-640 by yuli at 20220420 end*/
#endif

    fn_data.gdev->ts_stat = TS_SUSPEND;

    GTP_DEBUG("TPD suspend end.");
}

static int __init tpd_driver_init(void)
{
    /*hs04 code for DEAL6398A-1067 by tangsumian at 20220820 start*/
    if (NULL != saved_command_line && NULL != strstr(saved_command_line, "gc7202")){
        GTP_DEBUG("it is GC lcd,tpd_driver_init start.\n");
        /*HS04_U code for SR-AL6398U-01-3  by zhengkunbang at 20230807 start*/
        if ((tp_get_boot_mode() != NORMAL_BOOT) && (tp_get_boot_mode() != ALARM_BOOT)) {
            GTP_ERROR("tp init fail because boot_mode = %d\n",tp_get_boot_mode());
            return -EINVAL;
        }
        /*HS04_U code for SR-AL6398U-01-3  by zhengkunbang at 20230807 end*/
        tpd_get_dts_info();
        if (tpd_driver_add(&tpd_device_driver) < 0) {
            GTP_ERROR("add generic driver failed\n");
            return -ENODEV;
        }
    } else {
        GTP_ERROR("the command_line is NULL or it is not gc7202 lcd\n");
        return -ENODEV;
    }
    /*hs04 code for DEAL6398A-1067 by tangsumian at 20220820 end*/
    return 0;
}

/* should never be called */
static void __exit tpd_driver_exit(void)
{
    GTP_DEBUG("tpd_driver_exit exit\n");
    tpd_driver_remove(&tpd_device_driver);
}

module_init(tpd_driver_init);
module_exit(tpd_driver_exit);

MODULE_AUTHOR("GalaxyCore, Inc.");
MODULE_DESCRIPTION("GalaxyCore Touch Main Mudule");
MODULE_LICENSE("GPL");
