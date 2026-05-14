# 极简 Makefile：编译并运行 kvof_get UT。
#   make          # 编译
#   make test     # 运行 UT
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
TARGET   := kvof_get_ut
SRCS     := kvof_get.cpp kvof_get_ut.cpp

$(TARGET): $(SRCS) kvof_get.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)

.PHONY: test
test: $(TARGET)
	./$(TARGET)

.PHONY: clean
clean:
	rm -f $(TARGET)
