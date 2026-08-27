all: rom_and_ram.gb ppu_scanline_bgp.gb stop_instr.gb stop_instr_gbc_mode3.gb speed_switch_timing_div.gbc speed_switch_timing_ly.gbc speed_switch_timing_stat.gbc

clean:
	rm -rf *.o *.gb *.gbc

%.gb: %.o
	rgblink $< -o $@
	rgbfix -v $@ -p 0xff

%.gbc: %.o
	rgblink $< -o $@
	rgbfix -C -v $@ -p 0xff -t "TEST"

.PRECIOUS: %.o
%.o: %.asm common.inc hardware.inc
	rgbasm $< -o $@
