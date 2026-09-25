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

# The SGB child acid_test starts for --sgb1-bios/--sgb2-bios runs: the whole
# SNES core, so the real Super Game Boy BIOS runs each SGB test (acidsgb.h).
# ALLOW_CPU_OVERCLOCK as in the win32 build, so both children run the same core.
CHILD_OBJ   := obj_child
CHILD_CC    ?= gcc
ZDIR        := $(ROOT)/win32/zlib/src
CHILD_FLAGS := -std=gnu++17 -O2 -w -MMD -MP -DUNZIP_SUPPORT -DALLOW_CPU_OVERCLOCK
CHILD_INCS  := -I$(ROOT) -I$(ROOT)/apu -I$(ROOT)/apu/bapu -I$(ROOT)/filter \
               -I$(ROOT)/unzip -I$(SGBDIR) -I$(ROOT)/external/stb -I$(ZDIR)
CORE_NAMES  := acidsgb biosmanager bml bsx c4 c4emu cartprot cheats cheats2 clip \
               conffile controls cpu cpuexec cpuops crosshairs dma dsp dsp1 dsp2 \
               dsp3 dsp4 fscompat fxemu fxinst gfx globals hd64180 loadzip memmap \
               movie msu1 nss obc1 ppu sa1 sa1cpu screenshot sdd1 sdd1emu seta \
               seta010 seta011 seta018 sfcbox sha256 snapshot snes9x spc7110 srtc superdisc \
               statemanager stream tile tileimpl-h2x1 tileimpl-n1x1 tileimpl-n2x1 \
               voicekun widescreen z80 rp2040 rp2040cart
CHILD_CXX   := $(wildcard $(addprefix $(ROOT)/,$(addsuffix .cpp,$(CORE_NAMES)))) \
               $(ROOT)/apu/apu.cpp $(ROOT)/apu/bapu/dsp/sdsp.cpp \
               $(ROOT)/apu/bapu/smp/smp.cpp $(ROOT)/apu/bapu/smp/smp_state.cpp \
               $(SGBSRCS) $(ROOT)/external/stb/stb_image_implementation.cpp \
               $(SGBDIR)/tests/child_stubs.cpp
CHILD_C     := $(ROOT)/unzip/unzip.c $(ROOT)/unzip/ioapi.c \
               $(addprefix $(ZDIR)/,adler32.c compress.c crc32.c deflate.c gzclose.c \
               gzlib.c gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c \
               trees.c uncompr.c zutil.c)
child_obj    = $(CHILD_OBJ)/$(subst /,_,$(patsubst $(ROOT)/%,%,$(basename $(1)))).o
CHILD_OBJS  := $(foreach f,$(CHILD_CXX) $(CHILD_C),$(call child_obj,$(f)))

define CHILD_CXX_RULE
$(call child_obj,$(1)): $(1) | $(CHILD_OBJ)
	$$(CXX) $$(CHILD_FLAGS) $$(CHILD_INCS) -c $$< -o $$@
endef
define CHILD_C_RULE
$(call child_obj,$(1)): $(1) | $(CHILD_OBJ)
	$$(CHILD_CC) -O2 -w -MMD -MP -DUSE_FILE32API '-D_wopen(p,o,m)=(-1)' $$(CHILD_INCS) -c $$< -o $$@
endef
$(foreach f,$(CHILD_CXX),$(eval $(call CHILD_CXX_RULE,$(f))))
$(foreach f,$(CHILD_C),$(eval $(call CHILD_C_RULE,$(f))))

$(CHILD_OBJ):
	mkdir -p $@

acid_sgb_child: $(CHILD_OBJS)
	$(CXX) -o $@ $(CHILD_OBJS)

-include $(CHILD_OBJS:.o=.d)

clean:
	rm -f $(TARGET) acid_test acid_sgb_child sgb_out.ppm sgb_out.rgb
	rm -rf $(CHILD_OBJ)

.PHONY: all clean
