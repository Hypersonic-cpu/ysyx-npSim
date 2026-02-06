DEBUG_MODE?=0
ifeq ($(DEBUG_MODE),1)
	DBG_FLAGS = 
else
	DBG_FLAGS = -DNDEBUG
endif

CXX := clang++-22
# Build runnable simulator (do not compile libapi)
CXXFLAGS ?= -std=c++23 -stdlib=libc++ -O3 -flto -g -fPIC -I./src -Wall -Wno-reorder-ctor
CXXFLAGS += -I $(NPC_HOME)/libs/json/include
CXXFLAGS += -I $(NPC_HOME)/rvproc/sim-cxx/include
CXXFLAGS += -D ACTIVE_MODE=1 $(DBG_FLAGS)
SRCS_BRANCH := $(shell find "./src/branchSim" -name '*.cc' -type f)
SRCS_CACHE  := $(shell find "./src/cacheSim" -name '*.cc' -type f)
SRCS_PIPE   := $(shell find "./src/pipeSim" -name '*.cc' -type f)
SRCS_DEFINE := $(shell find "./src/defines" -name '*.cc' -type f)
SRCS_TRACE  := $(shell find "./src" -maxdepth 1 -name 'trace.cc' -type f)
SRCS_PMEM   := $(shell find "./src" -maxdepth 1 -name 'pmem.cc' -type f)

SRCS := $(SRCS_CACHE) $(SRCS_BRANCH) $(SRCS_PIPE) $(SRCS_TRACE) $(SRCS_DEFINE) $(SRCS_SDRAM) src/main.cc
# OBJS := $(SRCS:.cc=.o)
OBJS := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS:.cc=.o)))
OBJS_CACHE  := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS_CACHE:.cc=.o)))
OBJS_DEFINE := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS_DEFINE:.cc=.o)))
OBJS_TRACE  := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS_TRACE:.cc=.o)))
OBJS_PMEM   := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS_PMEM:.cc=.o)))

OBJS_CTEST := $(OBJS_TRACE) $(OBJS_DEFINE) $(OBJS_CACHE) $(OBJS_PMEM)

.PHONY: default clean all

default:
	@echo EXE $(EXEMODE)
	@echo SRCS $(CSRCS)

all: build/npsim.elf

build/npsim.elf: $(OBJS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

build/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/cacheTest/%.o: tests/cacheTest/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/cacheTest/%: $(OBJS_TRACE) $(OBJS_DEFINE) $(OBJS_CACHE) $(OBJS_PMEM) build/tests/cacheTest/%.o
	$(CXX) $(CXXFLAGS) $^ -o $@

.PHONY: cachetest test-cache test-all

cachetest: build/tests/cacheTest/test_cache build/tests/cacheTest/test_cache_advanced build/tests/cacheTest/test_cache_timing build/tests/cacheTest/test_cache_multiple
	@mkdir -p build
	@mv build/tests/cacheTest/test_cache build/cachetest
	@mv build/tests/cacheTest/test_cache_advanced build/cachetest-advanced
	@mv build/tests/cacheTest/test_cache_timing build/cachetest-timing
	@mv build/tests/cacheTest/test_cache_multiple build/cachetest-multiple

build/%.o: ./src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo $(OBJS)
	rm -rf build/*
