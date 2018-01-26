LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := greybus
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := build-greybus
include $(BUILD_PHONY_PACKAGE)

GB_SRC_PATH := $(LOCAL_PATH)
GB_KDIRARG := KERNELDIR="${ANDROID_PRODUCT_OUT}/obj/KERNEL_OBJ"

ifeq ($(TARGET_KERNEL_CLANG_COMPILE),true)
ifneq ($(TARGET_KERNEL_CLANG_VERSION),)
    # Find the clang-* directory containing the specified version
    KERNEL_CLANG_VERSION := $(shell find $(ANDROID_BUILD_TOP)/prebuilts/clang/host/$(HOST_OS)-x86/ -name AndroidVersion.txt -exec grep -l $(TARGET_KERNEL_CLANG_VERSION) "{}" \; | sed -e 's|/AndroidVersion.txt$$||g;s|^.*/||g')
else
    # Only set the latest version of clang if TARGET_KERNEL_CLANG_VERSION hasn't been set by the device config
    KERNEL_CLANG_VERSION := $(shell ls -d $(ANDROID_BUILD_TOP)/prebuilts/clang/host/$(HOST_OS)-x86/clang-* | xargs -n 1 basename | tail -1)
endif
TARGET_KERNEL_CLANG_PATH ?= $(ANDROID_BUILD_TOP)/prebuilts/clang/host/$(HOST_OS)-x86/$(KERNEL_CLANG_VERSION)/bin
ifeq ($(KERNEL_ARCH),arm64)
    KERNEL_CLANG_TRIPLE ?= CLANG_TRIPLE=aarch64-linux-gnu-
else ifeq ($(KERNEL_ARCH),arm)
    KERNEL_CLANG_TRIPLE ?= CLANG_TRIPLE=arm-linux-gnu-
else ifeq ($(KERNEL_ARCH),x86)
    KERNEL_CLANG_TRIPLE ?= CLANG_TRIPLE=x86_64-linux-gnu-
endif

KERNEL_CROSS_COMPILE := CROSS_COMPILE="$(KERNEL_TOOLCHAIN_PATH)"
KERNEL_CC ?= CC="$(ccache) $(TARGET_KERNEL_CLANG_PATH)/clang"

GB_KERNEL_TOOLS_PREFIX := $(KERNEL_TOOLCHAIN_PATH)

else
TARGET_KERNEL_CROSS_COMPILE_PREFIX := $(strip $(TARGET_KERNEL_CROSS_COMPILE_PREFIX))
ifeq ($(TARGET_KERNEL_CROSS_COMPILE_PREFIX),)
GB_KERNEL_TOOLS_PREFIX := arm-eabi-
else
GB_KERNEL_TOOLS_PREFIX := $(TARGET_KERNEL_CROSS_COMPILE_PREFIX)
endif
endif # TARGET_KERNEL_CLANG_COMPILE

GB_ARCHARG := ARCH=$(TARGET_ARCH)
GB_FLAGARG := EXTRA_CFLAGS+=-fno-pic
GB_ARGS := $(GB_KDIRARG) $(GB_ARCHARG) $(GB_FLAGARG)
GB_ARGS += $(KERNEL_CLANG_TRIPLE) $(KERNEL_CC)

#Create vendor/lib/modules directory if it doesn't exist
$(shell mkdir -p $(TARGET_OUT_VENDOR)/lib/modules)

ifeq ($(GREYBUS_DRIVER_INSTALL_TO_KERNEL_OUT),true)
GB_MODULES_OUT := $(KERNEL_MODULES_OUT)
else
GB_MODULES_OUT := $(TARGET_OUT_VENDOR)/lib/modules/
endif

build-greybus: $(ACP) $(INSTALLED_KERNEL_TARGET)
	$(MAKE) clean -C $(GB_SRC_PATH)
	$(MAKE) -j$(MAKE_JOBS) -C $(GB_SRC_PATH) CROSS_COMPILE=$(GB_KERNEL_TOOLS_PREFIX) $(GB_ARGS)
	ko=`find $(GB_SRC_PATH) -type f -name "*.ko"`;\
	for i in $$ko;\
	do $(GB_KERNEL_TOOLS_PREFIX)strip --strip-unneeded $$i;\
	$(ACP) -fp $$i $(GB_MODULES_OUT);\
	done
