AUTOMAKE_OPTIONS = foreign

# ========== Libtool 便利库（内部使用，不安装）==========
# 优势：公共代码只编译一次，支持未来扩展为共享库
noinst_LTLIBRARIES = libcomponents.la

libcomponents_la_SOURCES = \
    source/display/LinuxFramebufferDevice.cpp \
    source/display/TacoVOContext.cpp \
    source/display/TacoVODisplayDevice.cpp \
    source/display/SharedDisplayContext.cpp \
    source/display/SharedFramebufferDevice.cpp \
    source/display/DisplayDeviceFactory.cpp \
    source/productionline/worker/WorkerBase.cpp \
    source/productionline/worker/BufferFillingWorkerFacade.cpp \
    source/productionline/worker/BufferFillingWorkerFactory.cpp \
    source/productionline/worker/WorkerConfig.cpp \
    source/productionline/worker/FFmpegDecodeWorker.cpp \
    source/productionline/worker/FfmpegPacketRecorderWorker.cpp \
    source/productionline/worker/FFmpegEncodeWorker.cpp \
    source/productionline/worker/EncodedPacketSourceFromFile.cpp \
    source/productionline/worker/EncodedPacketSourceFromBuffer.cpp \
    source/productionline/worker/EncodedPacketSourceFromRtsp.cpp \
    source/productionline/worker/RawFrameSourceFromFile.cpp \
    source/productionline/worker/RawFrameSourceFromBuffer.cpp \
    source/monitor/PerformanceMonitor.cpp \
    source/common/Timer.cpp \
    source/common/GlobalThreadPool.cpp \
    source/buffer/bufferpool/Buffer.cpp \
    source/buffer/BufferAllocatorFactory.cpp \
    source/buffer/BufferAllocatorFacade.cpp \
    source/buffer/BufferAllocatorBase.cpp \
    source/buffer/NormalAllocator.cpp \
    source/buffer/FramebufferAllocator.cpp \
    source/buffer/AVFrameAllocator.cpp \
    source/buffer/bufferpool/BufferPool.cpp \
    source/buffer/bufferpool/BufferPoolRegistry.cpp \
    source/productionline/VideoProductionLine.cpp \
    source/productionline/MultiWorkerProductionLine.cpp \
    source/productionline/WorkerSyncCoordinator.cpp \
    source/productionline/io/BufferWriter.cpp \
    source/productionline/io/BufferComparator.cpp \
    source/productionline/io/BufferConsumerStrategies.cpp \
    source/productionline/io/BufferConsumerService.cpp

# ========== 测试程序 ==========
# 注意：display_test 已废弃（原 test_cases/dec/ 目录已重命名为 old_case/）
# bin_PROGRAMS = display_test

# ========== 公共链接库（定义为变量，便于维护）==========
# 注意：log4cplus 使用动态链接
# 在 Buildroot 交叉编译环境中：
# 1. Buildroot 会自动编译 log4cplus（已在 components.mk 中声明依赖）
# 2. log4cplus 库位置：$(STAGING_DIR)/usr/lib/liblog4cplus-2.0.so.3.4.7
# 3. 运行时需要目标系统有 log4cplus 动态库（Buildroot 会自动安装到 TARGET_DIR）
COMMON_LIBS = -lpthread -luring -lavformat -lavcodec -lavutil -lswscale -ltacosys -ltacovo -ltacocv -llog4cplus

# display_test 主程序（已废弃，原 test_cases/dec/ 目录已重命名为 old_case/）
# display_test_SOURCES = test_cases/dec/test.cpp
# display_test_CPPFLAGS = -I$(top_srcdir)/test_cases $(AM_CPPFLAGS)
# display_test_LDADD = libcomponents.la $(COMMON_LIBS)

# Google Test 程序（需要安装 libgtest-dev）
# bin_PROGRAMS += google_test
# google_test_SOURCES = \
#     test_cases/main.cpp \
#     test_cases/demo_google/test_ffmpeg_decode.cpp
# google_test_LDADD = -lgtest -lgtest_main -lpthread libcomponents.la $(COMMON_LIBS)
# google_test_CPPFLAGS = -I$(top_srcdir)/test_cases $(AM_CPPFLAGS)
# google_test_LDFLAGS = 

# ========== QA 测试程序（模块化测试入口）==========
bin_PROGRAMS = qa_cases
qa_cases_SOURCES = \
    test_cases/test_module_main.cpp \
    test_cases/common/ITestModule.cpp \
    test_cases/vdec/VdecTestSuite.cpp \
    test_cases/pp/PPTestSuite.cpp \
    test_cases/record/RecordTestSuite.cpp \
    test_cases/writer/WriterTestSuite.cpp
qa_cases_LDADD = libcomponents.la $(COMMON_LIBS)
qa_cases_CPPFLAGS = -I$(top_srcdir)/test_cases $(AM_CPPFLAGS)
qa_cases_LDFLAGS = -Wl,--allow-shlib-undefined

# ========== 编译选项 ==========
AM_CPPFLAGS = -I$(top_srcdir)/include

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif
