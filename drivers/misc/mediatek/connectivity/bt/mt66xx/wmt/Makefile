###############################################################################
# Bluetooth character device driver

###############################################################################
# Necessary Check

ifndef TOP
    TOP := $(srctree)/..
endif

#ifneq ($(CFG_BT_PM_QOS_CONTROL),)
#    ccflags-y += -D PM_QOS_CONTROL=1
#else
#    ccflags-y += -D PM_QOS_CONTROL=0
#endif

# Force build fail on modpost warning
KBUILD_MODPOST_FAIL_ON_WARNINGS := y

# platform
ccflags-y += -D CONNAC1X_CHIPID=$(BT_PLATFORM)
$(info $(LOG_TAG) BT_PLATFORM = $(BT_PLATFORM))
$(info $(LOG_TAG) srctree = $(srctree))


###############################################################################
# To add WMT dependent Macro and header file, will be removed later
$(info $(LOG_TAG) BT_ENABLE_LOW_POWER_DEBUG=$(BT_ENABLE_LOW_POWER_DEBUG))
ifeq ($(BT_ENABLE_LOW_POWER_DEBUG),y)
    ccflags-y += -D ENABLE_LOW_POWER_DEBUG=1
else
    ccflags-y += -D ENABLE_LOW_POWER_DEBUG=0
endif

ccflags-y += -D MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
#ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/mt$(BT_PLATFORM)/include

ifeq ($(CONFIG_MTK_CONN_LTE_IDC_SUPPORT),y)
    ccflags-y += -D WMT_IDC_SUPPORT=1
else
    ccflags-y += -D WMT_IDC_SUPPORT=0
endif

###############################################################################
# To include BT driver dependent header file

ccflags-y += -I$(WMT_SRC_FOLDER)/common_main/include
ccflags-y += -I$(WMT_SRC_FOLDER)/common_main/linux/include
ccflags-y += -I$(src)/../include/btif

###############################################################################

ifneq ($(BT_PLATFORM),)
MODULE_NAME := bt_drv_$(BT_PLATFORM)
else
MODULE_NAME := bt_drv
endif

obj-y += $(MODULE_NAME).o

ccflags-y += -D CREATE_NODE_DYNAMIC=1

$(MODULE_NAME)-objs += stp_chrdev_bt.o
$(MODULE_NAME)-objs += btmtk_dbg.o