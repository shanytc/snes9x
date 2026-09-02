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

# All sgb/ sources except the test itself. sgb/acid.cpp decodes reference
# PNGs through stb_image, so its implementation TU rides along, and
# sgb/sgbc_patches.cpp hashes the SGB2 BIOS, so the host's sha256 does too.
SGBSRCS := $(filter-out $(SGBDIR)/tests/%.cpp,$(wildcard $(SGBDIR)/*.cpp))
STBSRC  := $(ROOT)/external/stb/stb_image_implementation.cpp $(ROOT)/sha256.cpp
INCS    := -I$(ROOT) -I$(SGBDIR) -I$(ROOT)/external/stb

TARGET := sgb_test
SRCS   := cpu_test.cpp host_stubs.cpp

all: $(TARGET)

$(TARGET): $(SRCS) $(SGBSRCS)
	$(CXX) $(CXXFLAGS) $(INCS) $(SRCS) $(SGBSRCS) $(STBSRC) -o $@

# Headless GB Emulator Shootout runner — see acid_test.cpp.
acid_test: acid_test.cpp host_stubs.cpp $(SGBSRCS)
	$(CXX) $(CXXFLAGS) $(INCS) acid_test.cpp host_stubs.cpp $(SGBSRCS) $(STBSRC) -o $@

clean:
	rm -f $(TARGET) acid_test sgb_out.ppm sgb_out.rgb

.PHONY: all clean
