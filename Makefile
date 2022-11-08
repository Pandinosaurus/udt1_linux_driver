NAME_MODULE=udt1cri_usb
PACKAGE_VERSION=0.1

FILES = LICENSE Makefile README.md udt1cri.sh $(NAME_MODULE).c dkms.conf
obj-m+=$(NAME_MODULE).o

KERNEL_UNAME ?= $(shell uname -r)
KERNEL_SRC ?= /lib/modules/$(KERNEL_UNAME)/build/
SRC := $(shell pwd)
DEPMOD := depmod -a

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean

modules_install: all
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install
	$(DEPMOD)	

run:
	modprobe --remove $(NAME_MODULE) || true
	$(MAKE) modules_install
	sudo modprobe $(NAME_MODULE)

run_auto: dkms udev_install

dkms:
	mkdir -p /usr/src/udt1_linux_driver-$(PACKAGE_VERSION)
	cp $(FILES) /usr/src/udt1_linux_driver-$(PACKAGE_VERSION)
	sudo dkms add udt1_linux_driver -v $(PACKAGE_VERSION)
	sudo dkms build udt1_linux_driver -v $(PACKAGE_VERSION)
	sudo dkms install udt1_linux_driver -v $(PACKAGE_VERSION)
	
remove_all:
	sudo dkms remove udt1_linux_driver/0.1 --all || true
	rm -rf /usr/src/udt1_linux_driver-$(PACKAGE_VERSION)
	rm -f /lib/udev/rules.d/98-$(NAME_MODULE).rules

udev_install:
	mkdir -p /usr/src/udt1_linux_driver-$(PACKAGE_VERSION)
	cp $(FILES) /usr/src/udt1_linux_driver-$(PACKAGE_VERSION)
	cp 98-$(NAME_MODULE).rules /lib/udev/rules.d
	chmod	+x /usr/src/udt1_linux_driver-$(PACKAGE_VERSION)/udt1cri.sh
	udevadm control --reload 
	
test:
	g++ -lgtest_main -lgtest -lpthread ./tests/mcba_tests.cpp -o ./tests/mcba_tests
	./tests/mcba_tests --gtest_break_on_failure

