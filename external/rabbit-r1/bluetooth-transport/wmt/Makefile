# SPDX-License-Identifier: GPL-2.0-only
# Fixed Rabbit R1 target recipe reconstructed from recorded compiler commands.
obj-m += bt_drv_connac1x.o
bt_drv_connac1x-objs := stp_chrdev_bt.o \
    btmtk_dbg.o \
    btmtk_dbg_tp_evt_if.o
ccflags-y += -D \
    FW_LOG_DEFAULT_ON=0 \
    -D \
    CONNAC1X_CHIPID=connac1x \
    -D \
    ENABLE_LOW_POWER_DEBUG=0 \
    -D \
    MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT \
    -I$(srctree)/drivers/misc/mediatek/include \
    -I$(srctree)/drivers/misc/mediatek/include/mt-plat \
    -D \
    WMT_IDC_SUPPORT=0 \
    -I$(src)/../wmt/common_main/include \
    -I$(src)/../wmt/common_main/linux/include \
    -I$(src)/../include/btif \
    -D \
    CREATE_NODE_DYNAMIC=1 \
    -I$(src)/../vendor-headers \
    -Wno-error=missing-prototypes \
    -Wno-error=strict-prototypes
