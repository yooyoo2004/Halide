include ../support/Makefile.inc

all: process

local_laplacian_gen: ../../ local_laplacian_gen.cpp
	$(MAKE) -C ../../ $(LIB_HALIDE)
	$(CXX) $(CXXFLAGS) local_laplacian_gen.cpp -g ../../$(LIB_HALIDE) -o local_laplacian_gen -lpthread -ldl -lz \
	$(LDFLAGS) $(LLVM_SHARED_LIBS)

local_laplacian.o: local_laplacian_gen
	./local_laplacian_gen

local_laplacian_arm.o: local_laplacian_gen
	HL_TARGET=arm-32-android-armv7s-renderscript-debug ./local_laplacian_gen

libs/armeabi-v7a/process: $(HALIDE_LIB) \
                         jni/process.cpp \
                         local_laplacian_arm.o local_laplacian_arm.h
	NDK_MODULE_PATH=../support/ndk_modules ndk-build libs/armeabi-v7a/process libs/armeabi-v7a/libstlport_shared.so

process: process.cpp local_laplacian.o
	$(CXX) $(CXXFLAGS) -Wall -O3 process.cpp local_laplacian.o -o process -lpthread -ldl $(PNGFLAGS) $(OPENGL_LDFLAGS)

out.png: process
	./process ../images/rgb.png 8 1 1 10 out.png

# Build rules for generating a visualization of the pipeline using HalideTraceViz
process_viz: local_laplacian_viz.o
	$(CXX) $(CXXFLAGS) -Wall -O3 process.cpp local_laplacian_viz.o -o process_viz -lpthread -ldl $(PNGFLAGS) $(CUDA_LDFLAGS) $(OPENCL_LDFLAGS) $(OPENGL_LDFLAGS)

../../bin/HalideTraceViz:
	$(MAKE) -C ../../ bin/HalideTraceViz

local_laplacian_viz.o: local_laplacian_gen
	HL_TRACE=3 ./local_laplacian_gen 6
	mv local_laplacian.o local_laplacian_viz.o

local_laplacian.avi: ../../bin/HalideTraceViz process_viz
	bash viz.sh

deploy: libs/armeabi-v7a/process
	adb push ../images/rgb.png /mnt/sdcard/
	adb push libs/armeabi-v7a/process /mnt/sdcard/
	adb push libs/armeabi-v7a/libstlport_shared.so /mnt/sdcard/

define DEPLOY_STEPS
su
mkdir /data/tmp
cd /data/tmp
pwd
cp /mnt/sdcard/process .
chmod 777 /data/tmp/process
cp /mnt/sdcard/libstlport_shared.so .
rm /mnt/sdcard/out-arm.png
LD_LIBRARY_PATH=. ./process /mnt/sdcard/rgb.png 8 1 1 3 /mnt/sdcard/out-arm.png
exit
exit
endef
export DEPLOY_STEPS

out-arm.png: deploy
	sh -c 'echo "$$DEPLOY_STEPS" | adb shell'
	adb pull /mnt/sdcard/out-arm.png
	echo "Done"

clean:
	rm -f process local_laplacian.o process_viz local_laplacian_viz.o local_laplacian_gen local_laplacian.avi
	rm -rf libs/ obj/ local_laplacian_gen.dSYM/ local_laplacian_arm.o local_laplacian_arm.h out-arm.png