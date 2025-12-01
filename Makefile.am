AUTOMAKE_OPTIONS = foreign

# ========== Libtool 便利库（内部使用，不安装）==========
# 优势：公共代码只编译一次，支持未来扩展为共享库
noinst_LTLIBRARIES = libcomponents.la

libcomponents_la_SOURCES = \
    source/display/LinuxFramebufferDevice.cpp \
    source/productionline/worker/facade/BufferFillingWorkerFacade.cpp \
    source/productionline/worker/implementation/MmapRawVideoFileWorker.cpp \
    source/productionline/worker/factory/BufferFillingWorkerFactory.cpp \
    source/productionline/worker/implementation/FfmpegDecodeRtspWorker.cpp \
    source/productionline/worker/implementation/FfmpegDecodeVideoFileWorker.cpp \
    source/productionline/worker/implementation/IoUringRawVideoFileWorker.cpp \
    source/monitor/PerformanceMonitor.cpp \
    source/monitor/Timer.cpp \
    source/buffer/Buffer.cpp \
    source/buffer/allocator/factory/BufferAllocatorFactory.cpp \
    source/buffer/allocator/facade/BufferAllocatorFacade.cpp \
    source/buffer/allocator/base/BufferAllocatorBase.cpp \
    source/buffer/allocator/implementation/NormalAllocator.cpp \
    source/buffer/allocator/implementation/FramebufferAllocator.cpp \
    source/buffer/allocator/implementation/AVFrameAllocator.cpp \
    source/buffer/BufferPool.cpp \
    source/buffer/BufferPoolRegistry.cpp \
    source/productionline/VideoProductionLine.cpp

# ========== 测试程序（每个只包含自己的主文件）==========
bin_PROGRAMS = display_test test01

# ========== 公共链接库（定义为变量，便于维护）==========
COMMON_LIBS = -lpthread -luring -lavformat -lavcodec -lavutil -lswscale -ltacosys

# display_test 主程序
display_test_SOURCES = test.cpp
display_test_LDADD = libcomponents.la $(COMMON_LIBS)

# test01 测试程序
test01_SOURCES = test_cases/dec/test01.cpp
test01_LDADD = libcomponents.la $(COMMON_LIBS)

# ========== 编译选项 ==========
AM_CPPFLAGS = -I$(top_srcdir)/include

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif
