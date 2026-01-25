EXEMODE ?= 0
NPSIMPATH = .
SRCS_COMMON := $(shell find "$(NPSIMPATH)/src/cacheSim" -name '*.cc' -type f)

ifeq ($(EXEMODE),1)
# Build runnable simulator (do not compile libapi)
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -fPIC -I./src -I../npc/libs/json/include
SRCS_BRANCH := $(shell find "$(NPSIMPATH)/src/branchSim" -name '*.cc' -type f)
SRCS_PIPE := $(shell find "$(NPSIMPATH)/src/pipeSim" -name '*.cc' -type f)
SRCS_TRACE := $(shell find "$(NPSIMPATH)/src" -maxdepth 1 -name 'trace.cc' -type f)
SRCS_DEBUG := $(shell find "$(NPSIMPATH)/src" -maxdepth 1 -name 'debug.cc' -type f)

SRCS := $(SRCS_COMMON) $(SRCS_BRANCH) $(SRCS_PIPE) $(SRCS_TRACE) $(SRCS_DEBUG) src/main.cc
# OBJS := $(SRCS:.cc=.o)
OBJS := $(patsubst ./src/%, build/%, $(patsubst src/%, build/%, $(SRCS:.cc=.o)))

else
# Append to NPC $(CSRCS)
CSRCS += $(SRCS_COMMON)
# src/libapi.cc
INC_PATH += "$(NPSIMPATH)/src"

endif

.PHONY: default clean all

default:
	@echo EXE $(EXEMODE)
	@echo COMN $(SRCS_COMMON)
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
	rm -rf build/*
