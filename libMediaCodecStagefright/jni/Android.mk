LOCAL_PATH := $(call my-dir)

GLOBAL_CFLAGS := -s -fvisibility=hidden -fvisibility-inlines-hidden -DLI_API='__attribute__((visibility("hidden")))'
GLOBAL_CFLAGS += -Os -fPIE -fPIC
GLOBAL_CFLAGS += -ffunction-sections -fdata-sections -Wno-psabi -Wno-multichar
GLOBAL_LDLAGS := -Wl,--gc-sections

LIB_NAME :=MediaCodecStagefright
LIB_FILES :=StagefrightDecoder.cpp
LIB_PRIVATE_LIBS := -L$(ANDROID_LIBS) -lstagefright -lmedia -lutils -lbinder -lui -lcutils -llog
LIB_CFLAGS := $(GLOBAL_CFLAGS) -Wno-psabi -Wno-multichar

define BUILD_ONE_LIB
include $(CLEAR_VARS)
LOCAL_MODULE     := $(LIB_NAME)$(1)
LOCAL_SRC_FILES  := $(LIB_FILES)
LOCAL_C_INCLUDES := \
$(ANDROID_SYS_HEADERS_$(1))/frameworks/av/include \
$(ANDROID_SYS_HEADERS_$(1))/frameworks/native/include \
$(ANDROID_SYS_HEADERS_$(1))/frameworks/native/include/media/openmax \
$(ANDROID_SYS_HEADERS_$(1))/frameworks/base/include \
$(ANDROID_SYS_HEADERS_$(1))/frameworks/base/native/include \
$(ANDROID_SYS_HEADERS_$(1))/frameworks/base/include/media/stagefright/openmax \
$(ANDROID_SYS_HEADERS_$(1))/system/core/include \
$(ANDROID_SYS_HEADERS_$(1))/hardware/libhardware/include

LOCAL_CFLAGS     := $(LIB_CFLAGS) -DANDROID_$(1)
LOCAL_LDFLAGS    := $(GLOBAL_LDLAGS)
LOCAL_LDLIBS     := $(LIB_PRIVATE_LIBS)
include $(BUILD_SHARED_LIBRARY)
endef

$(foreach LIB, $(LIB_TARGETS), $(eval $(call BUILD_ONE_LIB,$(LIB))))