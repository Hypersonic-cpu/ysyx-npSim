EXEMODE ?= 0
NPSIMPATH = $(NPC_HOME)/../npsim
SRCS_COMMON := $(shell find "$(NPSIMPATH)/src/cacheSim" -name '*.cc' -type f)

ifeq ($(EXEMODE),1)
# Build runnable simulator (do not compile libapi)
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -fPIC -I./src
# $(shell find src/branchSim -name '*.cc' -type f)
SRCS := $(SRCS_COMMON) src/main.cc
OBJS := $(SRCS:.cc=.o)

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

all: npsim
npsim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# clean:
# 	rm -f npsim $(OBJS)
