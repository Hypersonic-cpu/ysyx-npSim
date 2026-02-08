
CXXFLAGS += -D ACTIVE_MODE=0
SRCS_BRANCH := $(shell find "$(NPSIM_HOME)/src/branchSim" -name '*.cc' -type f)
SRCS_CACHE  := $(shell find "$(NPSIM_HOME)/src/cacheSim" -name '*.cc' -type f)
SRCS_PIPE   := $(shell find "$(NPSIM_HOME)/src/pipeSim" -name '*.cc' -type f)
SRCS_DEFINE := $(shell find "$(NPSIM_HOME)/src/defines" -name '*.cc' -type f)
SRCS_TRACE  := $(shell find "$(NPSIM_HOME)/src" -maxdepth 1 -name 'trace.cc' -type f)
SRCS_PMEM   := $(shell find "$(NPSIM_HOME)/src" -maxdepth 1 -name 'pmem.cc' -type f)

# Append to NPC $(CSRCS)
CSRCS += $(SRCS_CACHE) $(SRCS_DEFINE)
# src/libapi.cc
INC_PATH += "$(NPSIM_HOME)/src"
