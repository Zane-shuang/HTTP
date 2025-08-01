# Makefile for Lightweight HTTP Server

# 编译器和编译选项
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread

# 目标文件和可执行文件
TARGET = http_server
SRCS = main.cpp server.cpp
OBJS = $(SRCS:.cpp=.o)

# 默认目标
all: $(TARGET)

# 链接目标文件生成可执行文件
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

# 编译源文件生成目标文件
%.o: %.cpp server.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 清理生成的文件
clean:
	rm -f $(OBJS) $(TARGET)

# 运行服务器
run:
	./$(TARGET)

.PHONY: all clean run