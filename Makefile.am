AUTOMAKE_OPTIONS = foreign

# ========== Libtool 便利库（内部使用，不安装）==========
# 优势：公共代码只编译一次，支持未来扩展为共享库
noinst_LTLIBRARIES = libcomponents.la

libcomponents_la_SOURCES = \
    source/vendor/taco/display/TacoDisplayContext.cpp \
    source/vendor/taco/display/TacoDisplayDevice.cpp \
    source/vendor/taco/display/TacoDisplayExtension.cpp \
    source/vendor/taco/display/TacoProOsdOverlay.cpp \
    source/vendor/taco/display/TacoProDisplayContext.cpp \
    source/vendor/taco/display/TacoProDisplayDevice.cpp \
    source/vendor/taco/display/TacoProDisplayExtension.cpp \
    source/vendor/taco/display/DisplayDeviceFactory.cpp \
    source/productionline/worker/base/WorkerBase.cpp \
    source/productionline/worker/base/WorkerFactory.cpp \
    source/productionline/worker/base/ComponentTopology.cpp \
    source/productionline/worker/config/WorkerConfig.cpp \
    source/productionline/worker/config/ConsumerTypeConfig.cpp \
    source/consumptionline/config/ConsumerTypeConfigBuilder.cpp \
    source/vendor/contracts/DecoderVendorRegistry.cpp \
    source/vendor/taco/decode/TacoDecoderExtension.cpp \
    source/vendor/taco/decode/TacoDecoderPluginLoader.cpp \
    source/vendor/taco/decode/TacoConfigBuilder.cpp \
    source/vendor/taco/encode/TacoEncoderExtension.cpp \
    source/productionline/worker/core/FFmpegDecodeWorker.cpp \
    source/productionline/worker/core/FfmpegPacketRecorderWorker.cpp \
    source/productionline/worker/core/FFmpegEncodeWorker.cpp \
    source/productionline/worker/datasource/encodeddata/EncodedPacketSourceFromFile.cpp \
    source/productionline/worker/datasource/encodeddata/EncodedPacketSourceFromBuffer.cpp \
    source/productionline/worker/datasource/encodeddata/EncodedPacketSourceFromRtsp.cpp \
    source/productionline/worker/datasource/rawdata/RawFrameSourceFromFile.cpp \
    source/productionline/worker/datasource/rawdata/RawFrameSourceFromBuffer.cpp \
    source/common/PerformanceMonitor.cpp \
    source/common/Timer.cpp \
    source/common/GlobalThreadPool.cpp \
    source/bufferpool/buffer/Buffer.cpp \
    source/bufferpool/buffer/AVFrameBuffer.cpp \
    source/bufferpool/buffer/MatBuffer.cpp \
    source/bufferpool/buffer/RawBuffer.cpp \
    source/common/ImageMeta.cpp \
    source/bufferpool/pool/base/BufferPool.cpp \
    source/bufferpool/pool/base/IBufferPoolBuilder.cpp \
    source/bufferpool/pool/builder/BufferPoolBuilderFactory.cpp \
    source/bufferpool/pool/builder/BufferPoolBuilder.cpp \
    source/vendor/MallocMemoryProvider.cpp \
    source/vendor/MemoryProviderRegistry.cpp \
    source/vendor/taco/TacoMemoryProvider.cpp \
    source/productionline/line/VideoProductionLine.cpp \
    source/productionline/line/MultiWorkerProductionLine.cpp \
    source/productionline/line/WorkerSyncCoordinator.cpp \
    source/consumptionline/BufferWriter.cpp \
    source/consumptionline/BufferComparator.cpp \
    source/consumptionline/BufferConsumerStrategies.cpp \
    source/consumptionline/BufferConsumerService.cpp \
    source/consumptionline/inference/NpuInferenceConsumer.cpp

# ========== 测试程序 ==========
# 注意：display_test 已废弃（原 test_cases/dec/ 目录已重命名为 old_case/）
# bin_PROGRAMS = display_test

# ========== 公共链接库（定义为变量，便于维护）==========
# 注意：log4cplus 使用动态链接
# 在 Buildroot 交叉编译环境中：
# 1. Buildroot 会自动编译 log4cplus（已在 components.mk 中声明依赖）
# 2. log4cplus 库位置：$(STAGING_DIR)/usr/lib/liblog4cplus-2.0.so.3.4.7
# 3. 运行时需要目标系统有 log4cplus 动态库（Buildroot 会自动安装到 TARGET_DIR）
COMMON_LIBS = -lpthread -ltacosys -ltavo -llog4cplus -l:libfreetype.so.6 $(FFMPEG_LIBS) $(OPENCV_LIBS) $(NPU_LIBS)
FFMPEG_LIBS = -lavformat -lavcodec -lavutil -lswscale
OPENCV_LIBS = -ltacocv -lvx -lhwdec_jpeg -ltaco_ppvdec -lCLC -lOpenVX -lOpenVXU -lOpenCL -lSPIRV_viv -lCLC -lVSC -lGAL -lovxlib -lArchModelSw -lNNArchPerf -ldl \
        -lopencv_imgcodecs -lopencv_videoio -lopencv_core -lopencv_imgproc -lenc24 -lyuv -ljpeg
NPU_LIBS = -lta_runtime -lVIPhal -lNBGlinker

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
bin_PROGRAMS = qa_cases webui_server

# ========== Mat(AVFrame*) 接口最小化测试 ==========
qa_cases_SOURCES = \
    test_cases/test_module_main.cpp \
    test_cases/common/ExecuteMode.cpp \
    test_cases/vdec/VdecPlugin.cpp \
    test_cases/pp/PPPlugin.cpp \
    test_cases/save/SavePlugin.cpp \
    test_cases/display/DisplayPlugin.cpp \
    test_cases/npu/NpuPlugin.cpp \
    test_cases/opencv/OpencvPlugin.cpp \
    test_cases/venc/VencPlugin.cpp \
    test_cases/preview/PreviewPlugin.cpp \
    test_cases/memleak/MemleakPlugin.cpp \
    test_cases/logconfig/LogConfigPlugin.cpp
qa_cases_LDADD = libcomponents.la $(COMMON_LIBS)
qa_cases_CPPFLAGS = -I$(top_srcdir)/test_cases $(AM_CPPFLAGS)
qa_cases_LDFLAGS = -L$(TARGET_DIR)/usr/local/lib -Wl,--no-as-needed -Wl,--allow-shlib-undefined

# ========== WebUI 服务器 ==========
# webui_server 复用 qa_cases 的插件（VdecPlugin, DisplayPlugin 等），
# 通过 CLI11 解析 API 参数 → 插件 applyTo → BufferConsumerService，
# 与 qa_cases 走完全一致的配置路径。
webui_server_SOURCES = \
    webui/backend/source/main.cpp \
    webui/backend/source/WebServer.cpp \
    webui/backend/source/ConfigStore.cpp \
    webui/backend/source/DataSourceManager.cpp \
    webui/backend/source/WorkerManager.cpp \
    webui/backend/source/ConsumerManager.cpp \
    webui/backend/source/PreviewService.cpp \
    webui/backend/source/ComponentsBridge.cpp \
    test_cases/common/ExecuteMode.cpp \
    test_cases/vdec/VdecPlugin.cpp \
    test_cases/display/DisplayPlugin.cpp \
    test_cases/npu/NpuPlugin.cpp \
    test_cases/preview/PreviewPlugin.cpp \
    test_cases/save/SavePlugin.cpp \
    test_cases/pp/PPPlugin.cpp \
    test_cases/opencv/OpencvPlugin.cpp \
    test_cases/venc/VencPlugin.cpp
webui_server_LDADD = libcomponents.la $(COMMON_LIBS)
webui_server_CPPFLAGS = -I$(top_srcdir)/webui/backend/include -I$(top_srcdir)/webui/backend/third_party -I$(top_srcdir)/test_cases $(AM_CPPFLAGS)
webui_server_LDFLAGS = -L$(TARGET_DIR)/usr/local/lib -Wl,--no-as-needed -Wl,--allow-shlib-undefined

# WebUI 前端静态文件安装到 /usr/local/share/webui/
webuistaticdir = $(prefix)/share/webui
dist_webuistatic_DATA =

# 安装前端预构建产物（如果 dist/ 目录存在）
install-data-local:
	@if [ -d "$(top_srcdir)/webui/dist" ]; then \
		$(MKDIR_P) "$(DESTDIR)$(webuistaticdir)"; \
		cp -r $(top_srcdir)/webui/dist/* "$(DESTDIR)$(webuistaticdir)/"; \
		echo "WebUI frontend installed to $(DESTDIR)$(webuistaticdir)"; \
	fi

# ========== 编译选项 ==========
OPENCV_INCLUDE = -I$(TARGET_DIR)/usr/local/include/opencv4
AM_CPPFLAGS = -I$(top_srcdir)/include $(OPENCV_INCLUDE) -I$(top_srcdir)/include/common/third_party/freetype2 -I$(TARGET_DIR)/usr/local/include/ta-runtime

if DEBUG
AM_CXXFLAGS = -g -O0 -Wall -std=c++17
else
AM_CXXFLAGS = -O2 -std=c++17
endif
