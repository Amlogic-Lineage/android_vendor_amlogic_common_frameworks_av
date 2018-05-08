LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_ARM_MODE := arm

LOCAL_MODULE:= libhpeqwrapper

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libdl \
    libutils \
    libamaudioutils

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    hardware/amlogic/audio/utils/ini/include

LOCAL_SRC_FILES += Hpeq.cpp

LOCAL_LDFLAGS_arm  += $(LOCAL_PATH)/libAmlHpeq.a

LOCAL_PRELINK_MODULE := false

LOCAL_LDLIBS   +=  -llog
ifeq ($(shell test $(PLATFORM_SDK_VERSION) -ge 26 && echo OK),OK)
LOCAL_PROPRIETARY_MODULE := true
endif

LOCAL_MODULE_RELATIVE_PATH := soundfx

include $(BUILD_SHARED_LIBRARY)
