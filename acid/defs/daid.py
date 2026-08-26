from test import *


all = [
    Test("daid/ppu_scanline_bgp.gb (DMG)", runtime=0.5, rom="daid/ppu_scanline_bgp.gb", result=["daid/ppu_scanline_bgp_0.dmg.png", "daid/ppu_scanline_bgp_1.dmg.png", "daid/ppu_scanline_bgp_2.dmg.png"], model=DMG,
        description="Mid scanline BGP register changes. Requires accurate PPU timing. Changing the BGP register has three possible effects for one pixel: the previous BGP is used, the next BGP is used, or the OR result of the previous and next BGP is used, the last case causing a black line. Which case occurs seems to be hardware and instance dependent as some DMGs do not do a consistent single case."),
    Test("daid/ppu_scanline_bgp.gb (GBC)", runtime=0.5, rom="daid/ppu_scanline_bgp.gb", result="daid/ppu_scanline_bgp.gbc.png", model=CGB,
        description="Mid scanline BGP register changes. Requires accurate PPU timing. Changing the BGP register will have the next BGP colors used."),
    Test("daid/stop_instr.gb (DMG)", runtime=0.5, rom="daid/stop_instr.gb", result="daid/stop_instr.dmg.png", model=DMG,
        description="STOP instruction is usually not used, but it should blank out the screen on classic Gameboy. As the PPU is stopped. NOTE: Running this on real hardware might damage the hardware, as the screen should be turned off before STOP on DMG."),
    Test("daid/stop_instr.gb (GBC)", runtime=0.5, rom="daid/stop_instr.gb", result="daid/stop_instr.gbc.png", model=CGB,
        description="STOP instruction is usually not used, but it should make the screen go black on Color Gameboy. The PPU is still running, but it cannot access VRAM, so it reads all black"),
    Test("daid/stop_instr_gbc_mode3.gb", runtime=0.5, model=CGB,
        description="STOP instruction is usually not used, but doing a STOP during mode 3 on Color Gameboy will keep the screen displaying the same data, as the PPU keeps running, and during mode3 it can access VRAM."),
    Test("daid/speed_switch_timing_div.gbc", runtime=0.5, model=CGB,
        description="Executing a STOP for a speed switch should reset the DIV register. Not doing this could cause problems with RNG for games, failing this will most certainly desync a TAS."),
    Test("daid/speed_switch_timing_ly.gbc", runtime=0.5, model=CGB),
    Test("daid/speed_switch_timing_stat.gbc", runtime=0.5, model=CGB),
    Test("daid/rom_and_ram.gb", runtime=0.5,
        description="Test how the ROM+RAM header option of the emulator acts. No official hardware is known to use this configuration. Most compatible way to emulate this is have always enabled RAM available.", url="https://github.com/daid/GBEmulatorShootout/blob/main/tests/daid/rom_and_ram.gb"),
]
