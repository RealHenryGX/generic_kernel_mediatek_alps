ccflags-y += -I$(srctree)/drivers/misc/mediatek/sensors-1.0/accelerometer/inc
ccflags-y += -I$(srctree)/drivers/misc/mediatek/sensors-1.0/hwmon/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/sensors-1.0/include
obj-y += mir3da.o
mir3da-y := mir3da_cust.o mir3da_core.o stock_pipeline.o
mir3da-$(CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS) += virtual_ps.o
