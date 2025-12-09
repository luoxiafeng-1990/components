AUTOMAKE_OPTIONS = foreign

# ========== Libtool 便利库（内部使用，不安装）==========
# 优势：公共代码只编译一次，支持未来扩展为共享库
noinst_LTLIBRARIES = libcomponents.la

libcomponents_la_SOURCES = \
    source/display/LinuxFramebufferDevice.cpp \
    source/productionline/worker/BufferFillingWorkerFacade.cpp \
    source/productionline/worker/MmapRawVideoFileWorker.cpp \
    source/productionline/worker/BufferFillingWorkerFactory.cpp \
    source/productionline/worker/FfmpegDecodeRtspWorker.cpp \
    source/productionline/worker/FfmpegDecodeVideoFileWorker.cpp \
    source/productionline/worker/IoUringRawVideoFileWorker.cpp \
    source/monitor/PerformanceMonitor.cpp \
    source/monitor/Timer.cpp \
    source/buffer/bufferpool/Buffer.cpp \
    source/buffer/BufferAllocatorFactory.cpp \
    source/buffer/BufferAllocatorFacade.cpp \
    source/buffer/BufferAllocatorBase.cpp \
    source/buffer/NormalAllocator.cpp \
    source/buffer/FramebufferAllocator.cpp \
    source/buffer/AVFrameAllocator.cpp \
    source/buffer/bufferpool/BufferPool.cpp \
    source/buffer/bufferpool/BufferPoolRegistry.cpp \
    source/productionline/VideoProductionLine.cpp

# ========== 测试程序（每个只包含自己的主文件）==========
bin_PROGRAMS = display_test test01

# ========== 公共链接库（定义为变量，便于维护）==========
# 注意：log4cplus 使用动态链接
# 在 Buildroot 交叉编译环境中：
# 1. Buildroot 会自动编译 log4cplus（已在 components.mk 中声明依赖）
# 2. log4cplus 库位置：$(STAGING_DIR)/usr/lib/liblog4cplus-2.0.so.3.4.7
# 3. 运行时需要目标系统有 log4cplus 动态库（Buildroot 会自动安装到 TARGET_DIR）
COMMON_LIBS = -lpthread -luring -lavformat -lavcodec -lavutil -lswscale -ltacosys -llog4cplus

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
