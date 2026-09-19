# ========================
# 编译器配置
# ========================
CXX = g++
CXXFLAGS = -std=c++17 -pthread -Wall -O2 -MMD -MP
.DEFAULT_GOAL := all

# ========================
# 目录配置
# ========================
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
BIN_DIR = bin

TARGET = $(BIN_DIR)/app

# ========================
# 源文件 & 目标文件
# ========================
# threadpool_test.cpp has its own main() and is not part of the server binary.
SRCS = $(filter-out $(SRC_DIR)/threadpool_test.cpp,$(wildcard $(SRC_DIR)/*.cpp))
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d)

-include $(DEPS)

# ========================
# 外部库（MySQL）
# ========================
LIBS = -lmysqlclient -lyaml-cpp

# Redis support is optional. When hiredis is available through pkg-config the
# cache implementation is compiled in; otherwise the server keeps its original
# in-memory behavior without adding a link-time dependency.
HIREDIS_CFLAGS := $(shell pkg-config --cflags hiredis 2>/dev/null)
HIREDIS_LIBS := $(shell pkg-config --libs hiredis 2>/dev/null)
ifneq ($(strip $(HIREDIS_LIBS)),)
CXXFLAGS += -DENABLE_REDIS $(HIREDIS_CFLAGS)
LIBS += $(HIREDIS_LIBS)
endif

# ========================
# 默认目标
# ========================
all: $(TARGET)

# ========================
# 链接
# ========================
$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# ========================
# 编译 .cpp -> .o
# ========================
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

# ========================
# 清理
# ========================
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# ========================
# 重新编译
# ========================
rebuild: clean
	$(MAKE) all

# ========================
# 运行
# ========================
run: all
	./$(TARGET)
