# Host-only build for the NPU<->CPU KV bridge unit tests.
#
#   make           -> build npu_kv_ut
#   make test      -> build and run
#   make clean
#
# This Makefile does NOT touch the Ascend C / CANN tool-chain. It just
# compiles the host service code and the CPU simulator. Use the sibling
# CMakeLists.txt (run.sh) to do a full NPU build.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  ?= -pthread

QUERY_DIR := ../query

OBJS = \
    npu_kv_host.o     \
    npu_kv_sim.o      \
    npu_kv_ut.o       \
    kv_client.o       \
    kv_server.o

all: npu_kv_ut

npu_kv_ut: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Local sources.
npu_kv_host.o: npu_kv_host.cpp npu_kv_host.h npu_kv_request.h $(QUERY_DIR)/kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

npu_kv_sim.o: npu_kv_sim.cpp npu_kv_sim.h npu_kv_request.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

npu_kv_ut.o: npu_kv_ut.cpp npu_kv_host.h npu_kv_sim.h npu_kv_request.h $(QUERY_DIR)/kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pull kv_transfer sources from the sibling query/ directory so we don't
# duplicate the metadata-query implementations.
kv_client.o: $(QUERY_DIR)/kv_client.cpp $(QUERY_DIR)/kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

kv_server.o: $(QUERY_DIR)/kv_server.cpp $(QUERY_DIR)/kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: npu_kv_ut
	./npu_kv_ut

clean:
	rm -f $(OBJS) npu_kv_ut

.PHONY: all test clean
