SRCS_COMMON := $(shell find "$(NPSIM_HOME)/src/cacheSim" -name '*.cc' -type f)
# Append to NPC $(CSRCS)
CSRCS += $(SRCS_COMMON)
# src/libapi.cc
INC_PATH += "$(NPSIM_HOME)/src"
