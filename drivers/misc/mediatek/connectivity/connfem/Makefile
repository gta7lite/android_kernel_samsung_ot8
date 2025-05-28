###############################################################################
# Necessary Check
###############################################################################

ifndef TOP
    TOP := $(srctree)/..
endif

# Force build fail on modpost warning
KBUILD_MODPOST_FAIL_ON_WARNINGS := y


###############################################################################
# GCC Options
###############################################################################
ccflags-y += -Wall
ccflags-y += -Werror


###############################################################################
# Compile Options
###############################################################################
ifneq ($(TARGET_BUILD_VARIANT), user)
    ccflags-y += -D CONNFEM_DBG=1
else
    ccflags-y += -D CONNFEM_DBG=0
endif


###############################################################################
# Include Paths
###############################################################################
ccflags-y += -I$(src)/include


###############################################################################
# ConnFem Module
###############################################################################
MODULE_NAME := connfem
obj-y += $(MODULE_NAME).o

$(MODULE_NAME)-objs += connfem_module.o
$(MODULE_NAME)-objs += connfem_api.o
$(MODULE_NAME)-objs += connfem_container.o
$(MODULE_NAME)-objs += connfem_dt_parser.o
$(MODULE_NAME)-objs += connfem_epaelna.o
$(MODULE_NAME)-objs += connfem_subsys_bt.o
$(MODULE_NAME)-objs += connfem_subsys_wifi.o

ccflags-y += -D CONNFEM_TEST_ENABLED=0