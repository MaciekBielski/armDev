# For kernel modules this makefile is called twice:
# - once normally by the user
# - second time from kernel build structure (KERNELRELEASE!='')
# more into examples directory

ifneq ($(KERNELRELEASE),)
	# call from kernel
	obj-m:= hello.o pl011_uart.o
	# module-objs:= file1.o file2.o
else
	# form command-line
	KDIR = ~/Coding/embb/zynq-2015.2/linux-xlnx
	SRCDIR := $(shell pwd)
	XCC := arm-linux-gnueabihf-
	TO_CLEAN := $(wildcard $(SRCDIR)/*.ko) $(wildcard $(SRCDIR)/*.o) \
		$(wildcard $(SRCDIR)/*.mod.c) $(wildcard $(SRCDIR)/.*.cmd)
endif

.PHONY: clean, default, send_ko, ultraclean

default:
	# change dir to KDIR first, remembering the current location M and run the
	# top-level makefile from that directory, which will call this makefile
	# again, add V=1 for more verbose output
	$(MAKE) -C $(KDIR) M=$(SRCDIR) ARCH=arm CROSS_COMPILE=$(XCC) modules

# how to compile user-space programs
hello_hf: helloworld.c
	arm-linux-gnueabihf-gcc -march=armv7-a -mtune=cortex-a9 \
	-mfloat-abi=softfp -std=gnu99 -o $@ $^

waiter: waiter.c
	arm-linux-gnueabihf-gcc -march=armv7-a -mtune=cortex-a9 \
	-mfloat-abi=softfp -std=gnu99 -o $@ $^

clean:
	@-rm -rf hello_hf
	@-rm -rf waiter
	@-rm -rf $(TO_CLEAN)
	@echo '[+] clean!'

ultraclean: clean
	@-rm -rf modules.order Module.symvers
	@echo '[+] ultraclean!'

send_ko:
	sshpass -p 'root' scp -P 2222 -o "UserKnownHostsFile=/dev/null" \
	-o "StrictHostKeyChecking=no" $(msg) root@localhost:
