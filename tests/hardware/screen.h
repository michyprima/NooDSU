// Text on the bottom screen's framebuffer and saving to the SD card or a flashcart, for the test ROMs
#include <stdio.h>

#define SCREEN ((volatile unsigned short*)0x06800000)
#define KEYS (*(volatile unsigned short*)0x04000130)
#define VCOUNT (*(volatile unsigned short*)0x04000006)

#define BIT_KEY(n) (1 << (n))
#define WHITE 0xFFFF
#define GREEN 0xC3E0
#define RED 0x801F
#define YELLOW 0x83FF
#define BLACK 0x8000

extern int textColumn, textRow;
void screenInit(void);
void text(const char *s, unsigned short color);
void hex(unsigned value, unsigned short color);
void number(int value, unsigned short color);
void newLine(void);
void clearFrom(int row);
FILE *openSave(const char *name, const char **where);
