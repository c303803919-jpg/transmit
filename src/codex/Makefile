CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

CPPFLAGS ?= -Ikvof_cpu

OBJS = kvof_cpu/mock.o kvof_cpu/token_get_index.o kvof_cpu/token_get_index_ut.o

all: token_get_index_ut

token_get_index_ut: $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(OBJS)

kvof_cpu/%.o: kvof_cpu/%.cpp kvof_cpu/kv_transfer.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

test: token_get_index_ut
	./token_get_index_ut

clean:
	rm -f $(OBJS) token_get_index_ut

.PHONY: all test clean
