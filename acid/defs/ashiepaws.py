from test import *


all = [
    Test("ashiepaws/bully.gb (DMG)", rom="ashiepaws/bully.gb", runtime=0.5,
        description="A collection of multiple test cases testing a variety of behaviors. (See Repository for Details)", url="https://github.com/Ashiepaws/BullyGB"),
    Test("ashiepaws/bully.gb (GBC)", rom="ashiepaws/bully.gb", runtime=0.5, model=CGB,
        description="A collection of multiple test cases testing a variety of behaviors. (See Repository for Details)", url="https://github.com/Ashiepaws/BullyGB"),
    Test("ashiepaws/strikethrough.gb", rom="ashiepaws/strikethrough.gb", runtime=0.5,
        description="Abuse of OAM DMA transfers during PPU modes 2 and 3 causing interference with data reads from the PPU."),
]
