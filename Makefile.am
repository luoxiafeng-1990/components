AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = display_test

display_test_SOURCES = test.cpp \
                       source/LinuxFramebufferDevice.cpp \
                       source/VideoFile.cpp \
                       source/PerformanceMonitor.cpp \
                       source/Timer.cpp \
                       source/BufferManager.cpp \
                       source/IoUringVideoReader.cpp

AM_CPPFLAGS = -I$(top_srcdir)/include

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif

LDADD = -lpthread -luring

