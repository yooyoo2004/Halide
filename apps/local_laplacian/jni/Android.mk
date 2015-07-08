LOCAL_PATH:= $(call my-dir)

# === process ===

include $(CLEAR_VARS)

LOCAL_MODULE           := process
LOCAL_SRC_FILES        := process.cpp
LOCAL_STATIC_LIBRARIES := android_native_app_glue
LOCAL_STATIC_LIBRARIES += libpng
LOCAL_LDLIBS           := -lm -llog -landroid -lz local_laplacian_arm.o
LOCAL_ARM_MODE         := arm

LOCAL_CPPFLAGS += -g -std=c++11 -I../support -I../../include

LOCAL_C_INCLUDES := ./

include $(BUILD_EXECUTABLE)

$(call import-module,android/native_app_glue)
$(call import-module,libpng)
