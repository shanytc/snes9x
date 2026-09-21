#ifndef _CARTPROT_H_
#define _CARTPROT_H_

// Security chip found on the unlicensed "Tekken 2" LoROM cart.
struct SCartProt
{
	uint8	Armed;
	uint8	Command;
	uint8	Argument;
	uint8	Answer;
};

extern struct SCartProt	CartProt;

bool8 S9xCartProtDetect (uint8 *rom, uint32 size);
void  S9xCartProtReset (void);
void  S9xCartProtWrite (uint32 address);
uint8 S9xCartProtRead (uint32 address);
void  S9xCartProtSetRomBase (uint32 bank, uint8 *base);

#endif
