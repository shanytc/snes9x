from test import *


all = [
    Test("acid/which.gb (DMG)", runtime=1.5, rom="acid/which.gb"),
    Test("acid/which.gb (GBC)", runtime=1.5, rom="acid/which.gb", model=CGB),
    Test("acid/dmg-acid2.gb", runtime=1.5,
        description="Rendering test for classic GameBoy.", url="https://github.com/mattcurrie/dmg-acid2"),
    Test("acid/cgb-acid2.gbc", runtime=1.5, model=CGB,
        description="Rendering test for color GameBoy.", url="https://github.com/mattcurrie/cgb-acid2"),
    Test("acid/cgb-acid-hell.gbc", runtime=1.5, model=CGB,
        description="Very specific rendering test of mid scanline writes, very hard to pass.", url="https://github.com/mattcurrie/cgb-acid-hell"),
]
