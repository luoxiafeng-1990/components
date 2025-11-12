AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = display_test

display_test_SOURCES = test.cpp \
                       source/display/LinuxFramebufferDevice.cpp \
                       source/videoFile/VideoFile.cpp \
                       source/monitor/PerformanceMonitor.cpp \
                       source/monitor/Timer.cpp \
                       source/buffer/BufferManager.cpp \
                       source/videoFile/IoUringVideoReader.cpp

AM_CPPFLAGS = -I$(top_srcdir)/include

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif

LDADD = -lpthread -luring

