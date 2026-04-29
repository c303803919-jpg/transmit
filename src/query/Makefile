# Build & run the KV-transfer unit tests.
#
#   make                         -> build both UTs
#   make test                    -> build and run both UTs
#   make kv_transfer_ut          -> legacy three-round flow
#   make token_get_index_ut      -> new Engine + token_get_index flow
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

LEGACY_OBJS = kv_client.o kv_server.o kv_transfer_ut.o
TGI_OBJS    = token_engine.o token_get_index_ut.o

all: kv_transfer_ut token_get_index_ut

# ---- legacy three-round flow (Client/Server) ----
kv_transfer_ut: $(LEGACY_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(LEGACY_OBJS)

# ---- new Engine + token_get_index flow ----
token_get_index_ut: $(TGI_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(TGI_OBJS)

%.o: %.cpp kv_transfer.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: all
	./kv_transfer_ut
	@echo
	./token_get_index_ut

clean:
	rm -f $(LEGACY_OBJS) $(TGI_OBJS) kv_transfer_ut token_get_index_ut

.PHONY: all test clean
