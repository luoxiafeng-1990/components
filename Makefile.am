AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = display_test

display_test_SOURCES = test.cpp \
                       source/display/LinuxFramebufferDevice.cpp \
                       source/videoFile/VideoFile.cpp \
                       source/videoFile/MmapVideoReader.cpp \
                       source/videoFile/VideoReaderFactory.cpp \
                       source/videoFile/RtspVideoReader.cpp \
                       source/videoFile/FfmpegVideoReader.cpp \
                       source/monitor/PerformanceMonitor.cpp \
                       source/monitor/Timer.cpp \
                       source/buffer/Buffer.cpp \
                       source/buffer/BufferAllocator.cpp \
                       source/buffer/BufferHandle.cpp \
                       source/buffer/BufferPool.cpp \
                       source/buffer/BufferPoolRegistry.cpp \
                       source/producer/VideoProducer.cpp \
                       source/videoFile/IoUringVideoReader.cpp \
                       source/decoder/Decoder.cpp \
                       source/decoder/FFmpegDecoder.cpp \
                       source/decoder/DecoderFactory.cpp

AM_CPPFLAGS = -I$(top_srcdir)/include

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif

LDADD = -lpthread -luring -lavformat -lavcodec -lavutil -lswscale -ltacosys

