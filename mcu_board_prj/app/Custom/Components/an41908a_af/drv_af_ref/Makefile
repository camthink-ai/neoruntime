ifeq ($(EXTDRV_PARAM_FILE), ) 
    EXTDRV_PARAM_FILE:=../Makefile.param
    include $(EXTDRV_PARAM_FILE)
endif

obj-m := drv_af.o 

hi_stepper-y := hi_drv_af.o

EXTRA_CFLAGS += $(DRV_CFLAGS)
EXTRA_CFLAGS += -I$(REL_INC) -I$(PUBLIC_INC_DIR)

all:
	@echo "999999999999"
	@echo $(KERNEL_ROOT)
	make -C $(KERNEL_ROOT) M=$(shell pwd) modules

clean:
	@make -C $(KERNEL_ROOT) M=$(shell pwd) clean
