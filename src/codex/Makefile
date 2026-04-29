CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

OBJS = token_engine.o kvof.o token_get_index_ut.o

all: token_get_index_ut

token_get_index_ut: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: token_get_index_ut
	./token_get_index_ut

clean:
	rm -f $(OBJS) token_get_index_ut

.PHONY: all test clean
