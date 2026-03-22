DEBUG_MODE?=1
ifeq ($(DEBUG_MODE),1)
	DBG_FLAGS = 
else
	DBG_FLAGS = -DNDEBUG
endif

SRC_PATH = ./src

SRCS_BRANCH := $(shell find "$(SRC_PATH)/branchSim" -name '*.cc' -type f)
SRCS_CACHE  := $(shell find "$(SRC_PATH)/cacheSim" -name '*.cc' -type f)
SRCS_PIPE   := $(shell find "$(SRC_PATH)/pipeSim" -name '*.cc' -type f)
SRCS_DEFINE := $(shell find "$(SRC_PATH)/defines" -name '*.cc' -type f)
SRCS_TRACE  := $(shell find "$(SRC_PATH)" -maxdepth 1 -name 'trace.cc' -type f)
SRCS_PMEM   := $(shell find "$(SRC_PATH)" -maxdepth 1 -name 'pmem.cc' -type f)


CXX := clang++-22
CXXFLAGS ?= -std=c++23 -stdlib=libc++ -O3 -flto -g -fPIC -I./src -Wall -Wno-reorder-ctor
CXXFLAGS += -I ./libs/json/include
CXXFLAGS += -I ./libs/stats_template
CXXFLAGS += -D ACTIVE_MODE=1 $(DBG_FLAGS)

SRCS := $(SRCS_CACHE) $(SRCS_BRANCH) $(SRCS_PIPE) $(SRCS_TRACE) $(SRCS_DEFINE) $(SRCS_SDRAM) src/main.cc

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

# cachetest: build/tests/cacheTest/test_cache build/tests/cacheTest/test_cache_advanced build/tests/cacheTest/test_cache_timing build/tests/cacheTest/test_cache_multiple
	# @mkdir -p build
	# @mv build/tests/cacheTest/test_cache build/cachetest
	# @mv build/tests/cacheTest/test_cache_advanced build/cachetest-advanced
	# @mv build/tests/cacheTest/test_cache_timing build/cachetest-timing
	# @mv build/tests/cacheTest/test_cache_multiple build/cachetest-multiple

build/%.o: ./src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo $(OBJS)
	rm -rf build/*


