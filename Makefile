CXX := clang++-22
# Build runnable simulator (do not compile libapi)
CXXFLAGS ?= -std=c++23 -stdlib=libc++ -O3 -fPIC -I./src
CXXFLAGS += -I $(NPC_HOME)/libs/json/include
CXXFLAGS += -I $(NPC_HOME)/rvproc/sim-cxx
CXXFLAGS += -D ACTIVE_MODE=1
SRCS_BRANCH := $(shell find "./src/branchSim" -name '*.cc' -type f)
SRCS_CACHE  := $(shell find "./src/cacheSim" -name '*.cc' -type f)
SRCS_PIPE   := $(shell find "./src/pipeSim" -name '*.cc' -type f)
SRCS_TRACE  := $(shell find "./src" -maxdepth 1 -name 'trace.cc' -type f)
SRCS_DEBUG  := $(shell find "./src" -maxdepth 1 -name 'debug.cc' -type f)
SRCS_SDRAM  := $(shell find "./src" -maxdepth 1 -name 'sdram.cc' -type f)

SRCS := $(SRCS_CACHE) $(SRCS_BRANCH) $(SRCS_PIPE) $(SRCS_TRACE) $(SRCS_DEBUG) $(SRCS_SDRAM) src/main.cc
# OBJS := $(SRCS:.cc=.o)
OBJS := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS:.cc=.o)))

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

build/%.o: ./src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo $(OBJS)
	rm -rf build/*
