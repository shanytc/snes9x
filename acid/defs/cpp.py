from test import *


all = [
    Test("cpp/rtc-invalid-banks-test.gb", rom="cpp/rtc-invalid-banks-test.gb", runtime=0.5,
        description="Tests how invalid banks are handled with MBC3 RTC."),
    Test("cpp/latch-rtc-test.gb", rom="cpp/latch-rtc-test.gb", runtime=0.5,
        description="Writes random values to RTC regs, reports them back, then latches the RTC using a single write to the 0x6000-0x7FFF region."),
    Test("cpp/ramg-mbc3-test.gb", rom="cpp/ramg-mbc3-test.gb", runtime=0.5,
        description="Tests the width of the MBC3's RAM gate register."),
    Test("cpp/sgb-ext-test.gb", rom="cpp/sgb-ext-test.gb", runtime=0.5, model=SGB, url="https://github.com/CasualPokePlayer/test-roms/tree/sgb-ext-test",
        description="Tests the SGB packet protocol, doing various \"stress tests\" which cannot be passed without correctly emulating the SGB packet protocol. (See repository for passing reference)"),
]
