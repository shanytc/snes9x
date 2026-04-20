# SGB standalone test harness.
#
# Links the sgb/ subsystem only — no snes9x host code is required.
# Linked sources are pulled from the sibling sgb/ directory so this
# Makefile lives self-contained in sgb/tests/.
#
# Typical usage:
#   make                                  # build ./sgb_test
#   ./sgb_test rom.gb 1800 sgb 1.0        # run 1800 frames in SGB1 mode
#   ./sgb_test cpu_instrs.gb 60000 dmg    # 60k-frame Blargg pass
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
ROOT     := $(abspath ../..)
SGBDIR   := $(ROOT)/sgb

# All sgb/ sources except the test itself.
SGBSRCS := $(filter-out $(SGBDIR)/tests/%.cpp,$(wildcard $(SGBDIR)/*.cpp))

TARGET := sgb_test
SRCS   := cpu_test.cpp

all: $(TARGET)

$(TARGET): $(SRCS) $(SGBSRCS)
	$(CXX) $(CXXFLAGS) -I$(ROOT) -I$(SGBDIR) $(SRCS) $(SGBSRCS) -o $@

clean:
	rm -f $(TARGET) sgb_out.ppm sgb_out.rgb

.PHONY: all clean
