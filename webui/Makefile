# Components WebUI - 辅助 Makefile
#
# 后端 C++ 编译由上层 autotools (Makefile.am + Buildroot) 处理
# 本 Makefile 仅用于：
#   make frontend       - 构建前端静态文件到 dist/
#   make frontend-dev   - 启动前端开发服务器
#   make clean-frontend - 清理前端产物
#   make backend-local  - 本地编译后端（开发调试用，非交叉编译）

DIST_DIR = dist

.PHONY: frontend frontend-dev clean-frontend backend-local clean-local run-local

# ===== 前端 =====

frontend:
	cd frontend && npm install && npm run build
	@echo "[WebUI] Frontend built to $(DIST_DIR)/"

frontend-dev:
	cd frontend && npm install && npm run dev

clean-frontend:
	rm -rf $(DIST_DIR)
	rm -rf frontend/node_modules

# ===== 本地开发编译（x86_64，不链接 TACO 硬件库）=====
# 仅用于开发调试 API 接口，不涉及硬件功能

CXX ?= g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
LDFLAGS = -pthread

BACKEND_SOURCES = \
	backend/source/main.cpp \
	backend/source/WebServer.cpp \
	backend/source/ConfigStore.cpp \
	backend/source/DataSourceManager.cpp \
	backend/source/WorkerManager.cpp \
	backend/source/ConsumerManager.cpp \
	backend/source/PreviewService.cpp \
	backend/source/PreviewSessionManager.cpp \
	backend/source/PreviewFrameTap.cpp \
	backend/source/ComponentsBridge.cpp

BUILD_DIR = build

backend-local: $(BUILD_DIR)/webui_server_local

$(BUILD_DIR)/webui_server_local: $(BACKEND_SOURCES) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) \
		-Ibackend/include -Ibackend/third_party \
		-o $@ $(BACKEND_SOURCES) $(LDFLAGS)
	@echo "[WebUI] Local backend built: $@"

# Host-side unit test for PreviewSessionManager (no TACO / FFmpeg link).
test-preview-session: $(BUILD_DIR)/PreviewSessionManagerTest
	./$(BUILD_DIR)/PreviewSessionManagerTest

$(BUILD_DIR)/PreviewSessionManagerTest: \
		backend/tests/PreviewSessionManagerTest.cpp \
		backend/source/PreviewSessionManager.cpp \
		backend/include/PreviewSessionManager.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DPREVIEW_SESSION_NO_FFMPEG \
		-Ibackend/include -Ibackend/third_party \
		-o $@ \
		backend/tests/PreviewSessionManagerTest.cpp \
		backend/source/PreviewSessionManager.cpp \
		$(LDFLAGS)
	@echo "[WebUI] PreviewSessionManagerTest built: $@"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run-local: $(BUILD_DIR)/webui_server_local frontend
	./$(BUILD_DIR)/webui_server_local --port 8080 --static $(DIST_DIR)

clean-local:
	rm -rf $(BUILD_DIR)
