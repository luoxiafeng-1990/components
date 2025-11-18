AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = display_test

display_test_SOURCES = test.cpp \
                       source/display/LinuxFramebufferDevice.cpp \
                       source/productionline/worker/BufferFillingWorker.cpp \
                       source/productionline/worker/MmapRawVideoFileWorker.cpp \
                       source/productionline/worker/BufferFillingWorkerFactory.cpp \
                       source/productionline/worker/FfmpegDecodeRtspWorker.cpp \
                       source/productionline/worker/FfmpegDecodeVideoFileWorker.cpp \
                       source/monitor/PerformanceMonitor.cpp \
                       source/monitor/Timer.cpp \
                       source/buffer/Buffer.cpp \
                       source/buffer/allocator/BufferAllocator.cpp \
                       source/buffer/allocator/NormalAllocator.cpp \
                       source/buffer/allocator/FramebufferAllocator.cpp \
                       source/buffer/allocator/AVFrameAllocator.cpp \
                       source/buffer/BufferPool.cpp \
                       source/buffer/BufferPoolRegistry.cpp \
                       source/productionline/VideoProductionLine.cpp \
                       source/productionline/worker/IoUringRawVideoFileWorker.cpp 
                       

AM_CPPFLAGS = -I$(top_srcdir)/include

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif

LDADD = -lpthread -luring -lavformat -lavcodec -lavutil -lswscale -ltacosys

