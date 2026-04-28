# Build & run the KV transfer UT.
#   make           -> build kv_transfer_ut
#   make test      -> build and run
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

OBJS = kv_client.o kv_server.o kv_transfer_ut.o

all: kv_transfer_ut

kv_transfer_ut: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: kv_transfer_ut
	./kv_transfer_ut

clean:
	rm -f $(OBJS) kv_transfer_ut

.PHONY: all test clean
