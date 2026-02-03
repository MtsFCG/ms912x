ms912x-y := \
	ms912x_registers.o \
	ms912x_connector.o \
	ms912x_transfer.o \
	ms912x_drv.o \
	ms912x_simd.o

# Enable AVX2 and O3 for the SIMD file.
# We also disable stack protector to avoid potential issues in this specific extensive optimized unit,
# though mostly it is standard C.
CFLAGS_ms912x_simd.o := -mavx2 -O3 -fno-stack-protector

obj-m := ms912x.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

all:	modules

modules:
	make CHECK="/usr/bin/sparse" -C $(KSRC) M=$(PWD) modules

clean:
	make -C $(KSRC) M=$(PWD) clean
	rm -f $(PWD)/Module.symvers $(PWD)/*.ur-safe
