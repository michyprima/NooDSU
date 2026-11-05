// Writes results to a file on the flashcart's card through its DLDI driver, like homebrew does, or else to the save chip
// Which chip a flashcart acts as depends on it, so each kind is tried, reading back to see what works
#include <stdio.h>
#include <stdbool.h>
#include "rom.h"

// From libfat and libgba, whose headers choose what they include by whether ARM9 is defined, as it is here
typedef struct DISC_INTERFACE_STRUCT DISC_INTERFACE;
extern const DISC_INTERFACE *dldiGetInternal(void);
extern bool fatMountSimple(const char *name, const DISC_INTERFACE *interface);
extern void fatUnmount(const char *name);
extern char *fake_heap_start, *fake_heap_end;
extern char __end__[];

// The memory the C library can use is what's left before the ARM7's code
#define HEAP_END ((char*)0x02180000)
#ifndef FILE_NAME
#define FILE_NAME "fat:/noodsu-results.bin"
#define FILE_LABEL "NOODSU-RESULTS.BIN"
#endif
#define FILE_LIMIT 0x800000

static FILE *file;
static unsigned fileHash, fileBytes;
static unsigned char fileBuffer[4096];
static int fileFill, fileFailed;

#define AUXSPICNT REG16(0x040001A0)
#define AUXSPIDATA (*(volatile unsigned char*)0x040001A2)
#define EXMEMCNT REG16(0x04000204)

static struct Chip { const char *name; int write, addressBytes, page, size; } chip;
static const struct Chip chips[] = {
    { "FLASH", 0x0A, 3, 256, 0x80000 }, // Page write, which needs no erasing
    { "FLASH", 0x02, 3, 256, 0x80000 }, // Page program, for flash that's already erased
    { "EEPROM", 0x02, 2, 128, 0x10000 },
    { "EEPROM", 0x02, 3, 256, 0x20000 },
    { "EEPROM", 0x02, 2, 32, 0x2000 },
};

static unsigned char spi(unsigned char value, int last) {
    AUXSPICNT = last ? 0xA000 : 0xA040;
    AUXSPIDATA = value;
    while (AUXSPICNT & 0x80);
    return AUXSPIDATA;
}

static void address(int value) {
    for (int i = chip.addressBytes - 1; i >= 0; i--)
        spi(value >> (i * 8), 0);
}

static int waitReady(void) {
    // Give up after a while, for a chip that isn't there
    for (int tries = 0; tries < 200000; tries++) {
        spi(0x05, 0);
        if (!(spi(0, 1) & 1)) return 1;
    }
    return 0;
}

static int writePage(int to, const unsigned char *data, int length) {
    spi(0x06, 1);
    spi(chip.write, 0);
    address(to);
    for (int i = 0; i < length; i++) spi(data[i], i == length - 1);
    return waitReady();
}

static void readBytes(int from, unsigned char *data, int length) {
    spi(0x03, 0);
    address(from);
    for (int i = 0; i < length; i++) data[i] = spi(0, i == length - 1);
}

static int writeVerified(int to, const unsigned char *data, int length) {
    static unsigned char back[256];
    if (!writePage(to, data, length)) return 0;
    readBytes(to, back, length);
    for (int i = 0; i < length; i++)
        if (back[i] != data[i]) return 0;
    return 1;
}

static int findChip(void) {
    // A word that won't be there already, from how long the DS has been on
    static unsigned char probe[32] = "NOODSUPROBE";
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        chip = chips[i];
        probe[12] = REG16(0x04000100);
        probe[13] = REG16(0x04000006);
        probe[14] = i;
        if (writeVerified(0, probe, 32)) return 1;
    }
    return 0;
}

static unsigned char pending[256];
static int pendingLength, writtenTo, failed;

static void saveByte(unsigned char value) {
    // Bytes past the chip's end are dropped, so the start of the results is what's kept
    if (failed || writtenTo + pendingLength >= chip.size) return;
    pending[pendingLength++] = value;
    if (pendingLength == chip.page) {
        if (writeVerified(writtenTo, pending, pendingLength)) writtenTo += pendingLength;
        else failed = 1;
        pendingLength = 0;
    }
}

static void fileFlush(void) {
    if (fileFill && fwrite(fileBuffer, 1, fileFill, file) != (size_t)fileFill) fileFailed = 1;
    fileFill = 0;
}

void saveWord(unsigned value) {
    for (int i = 0; i < 4; i++) {
        unsigned char byte = value >> (i * 8);
        if (!file) {
            saveByte(byte);
            continue;
        }
        fileHash = (fileHash ^ byte) * 16777619u;
        fileBytes++;
        fileBuffer[fileFill++] = byte;
        if (fileFill == sizeof(fileBuffer)) fileFlush();
    }
}

static int openFile(void) {
    // Only with a driver the flashcart patched in, and a card it can read
    fake_heap_start = __end__;
    fake_heap_end = HEAP_END;
    const DISC_INTERFACE *disc = dldiGetInternal();
    if (!disc || !fatMountSimple("fat", disc)) return 0;
    file = fopen(FILE_NAME, "wb");
    if (!file) fatUnmount("fat");
    return file != 0;
}

static int closeFile(void) {
    // Read it back from the card to be sure it's all there
    fileFlush();
    int good = !fileFailed && fclose(file) == 0;
    file = 0;
    FILE *back = good ? fopen(FILE_NAME, "rb") : 0;
    unsigned hash = 2166136261u, bytes = 0;
    if (back) {
        size_t got;
        while ((got = fread(fileBuffer, 1, sizeof(fileBuffer), back)) > 0) {
            for (size_t i = 0; i < got; i++) hash = (hash ^ fileBuffer[i]) * 16777619u;
            bytes += got;
        }
        fclose(back);
    }
    fatUnmount("fat");
    if (!back || hash != fileHash || bytes != fileBytes) return -(int)bytes;
    return bytes;
}

int saveStart(const char **name) {
    EXMEMCNT = EXMEMCNT & ~0x800; // The ARM9 gets the card
    pendingLength = writtenTo = failed = 0;
    fileHash = 2166136261u;
    fileBytes = fileFill = fileFailed = 0;
    if (openFile()) {
        *name = FILE_LABEL;
        return 1;
    }
    if (!findChip()) return 0;
    *name = chip.name;
    return 1;
}

int saveCapacity(void) { return file ? FILE_LIMIT : chip.size; }

int saveEnd(void) {
    if (file) return closeFile();

    // The rest of the last page, which keeps it a whole page for chips that need that
    while (pendingLength && !failed) saveByte(0xFF);
    return failed ? -writtenTo : writtenTo;
}
