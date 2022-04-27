/*
    Copyright 2019-2021 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include <cstring>

#include "cartridge.h"
#include "core.h"
#include "settings.h"

Cartridge::~Cartridge()
{
    // Write the save before exiting
    writeSave();

    // Free the ROM and save memory
    if (ndsRomFile) fclose(ndsRomFile);
    if (ndsRom)  delete[] ndsRom;
    if (ndsSave) delete[] ndsSave;
    if (gbaRom)  delete[] gbaRom;
    if (gbaSave) delete[] gbaSave;
}

void Cartridge::loadRomSection(uint32_t offset, uint32_t size)
{
    // Load a section of the current NDS ROM file into memory
    if (ndsRom) delete[] ndsRom;
    ndsRom = new uint8_t[size];
    fseek(ndsRomFile, offset, SEEK_SET);
    fread(ndsRom, sizeof(uint8_t), size, ndsRomFile);
}

void Cartridge::loadNdsRom(std::string path)
{
    // Attempt to load an NDS ROM
    ndsRomName = path;
    ndsRomFile = fopen(ndsRomName.c_str(), "rb");
    fseek(ndsRomFile, 0, SEEK_END);
    ndsRomSize = ftell(ndsRomFile);

    // If the ROM is 512MB or smaller, try to load it all into memory; otherwise fall back to file-based loading
    /*if (ndsRomSize <= 0x20000000) // 512MB
    {
        try
        {
            loadRomSection(0, ndsRomSize);
            fclose(ndsRomFile);
            ndsRomFile = nullptr;
        }
        catch (std::bad_alloc &ba)
        {
            loadRomSection(0, 0x5000);
        }
    }
    else*/
    {
        loadRomSection(0, 0x5000);
    }

    // Save the ROM code, which is mainly used for encryption
    ndsRomCode = U8TO32(ndsRom, 0x0C);

    // Check if the ROM is encrypted
    if (ndsRomSize >= 0x8000) // ROM has secure area
    {
        // Decrypt the 'encryObj' string
        uint64_t data = U8TO64(ndsRom, 0x4000);
        initKeycode(2);
        data = decrypt64(data);
        initKeycode(3);
        data = decrypt64(data);

        // If decryption was successful, the ROM is encrypted
        if (data == 0x6A624F7972636E65) // encryObj
        {
            LOG("Detected an encrypted ROM!\n");
            ndsRomEncrypted = true;
        }
    }

    // Attempt to load the ROM's save file
    ndsSaveName = path.substr(0, path.rfind(".")) + ".sav";
    FILE *ndsSaveFile = fopen(ndsSaveName.c_str(), "rb");
    if (ndsSaveFile)
    {
        fseek(ndsSaveFile, 0, SEEK_END);
        ndsSaveSize = ftell(ndsSaveFile);
        fseek(ndsSaveFile, 0, SEEK_SET);
        ndsSave = new uint8_t[ndsSaveSize];
        fread(ndsSave, sizeof(uint8_t), ndsSaveSize, ndsSaveFile);
        fclose(ndsSaveFile);
    }
    else
    {
        // The save size is unknown, so just assume FLASH 512KB and hope the user will change it if it doesn't work
        ndsSaveSize = -1;
    }
}

void Cartridge::loadGbaRom(std::string path)
{
    // Attempt to load a GBA ROM
    gbaRomName = path;
    FILE *gbaRomFile = fopen(gbaRomName.c_str(), "rb");
    //if (!gbaRomFile) throw 2;
    fseek(gbaRomFile, 0, SEEK_END);
    gbaRomSize = ftell(gbaRomFile);
    fseek(gbaRomFile, 0, SEEK_SET);
    gbaRom = new uint8_t[gbaRomSize];
    fread(gbaRom, sizeof(uint8_t), gbaRomSize, gbaRomFile);
    fclose(gbaRomFile);

    core->memory.updateMap9(0x08000000, 0x0A000000);
    core->memory.updateMap7(0x08000000, 0x0D000000);

    // Attempt to load the ROM's save file
    gbaSaveName = path.substr(0, path.rfind(".")) + ".sav";
    FILE *gbaSaveFile = fopen(gbaSaveName.c_str(), "rb");
    if (gbaSaveFile)
    {
        fseek(gbaSaveFile, 0, SEEK_END);
        gbaSaveSize = ftell(gbaSaveFile);
        fseek(gbaSaveFile, 0, SEEK_SET);
        gbaSave = new uint8_t[gbaSaveSize];
        fread(gbaSave, sizeof(uint8_t), gbaSaveSize, gbaSaveFile);
        fclose(gbaSaveFile);
    }
    else
    {
        const std::string saveStrs[] = { "EEPROM_V", "SRAM_V", "FLASH_V", "FLASH512_V", "FLASH1M_V" };
        int match = -1;

        // Unlike the DS, a GBA cart's save type can be reliably detected by searching for strings in the ROM
        // Search the ROM for a save string so a new save of that type can be created
        for (unsigned int i = 0; i < gbaRomSize; i += 4)
        {
            for (unsigned int j = 0; j < 5; j++)
            {
                match = j;
                for (unsigned int k = 0; k < saveStrs[j].length(); k++)
                {
                    if (i + k >= gbaRomSize || gbaRom[i + k] != saveStrs[j][k])
                    {
                        match = -1;
                        break;
                    }
                }
                if (match != -1) break;
            }
            if (match != -1) break;
        }

        // Create a new GBA save of the detected type
        if (match != -1)
        {
            switch (match)
            {
                case 0: // EEPROM
                {
                    // EEPROM can be either 0.5KB or 8KB, so it must be guessed based on how the game uses it
                    gbaSaveSize = -1;
                    return;
                }

                case 1: // SRAM 32KB
                {
                    gbaSaveSize = 0x8000;
                    LOG("Detected SRAM 32KB save type\n");
                    break;
                }

                case 2: case 3: // FLASH 64KB
                {
                    gbaSaveSize = 0x10000;
                    LOG("Detected FLASH 64KB save type\n");
                    break;
                }

                case 4: // FLASH 128KB
                {
                    gbaSaveSize = 0x20000;
                    LOG("Detected FLASH 128KB save type\n");
                    break;
                }
            }

            gbaSave = new uint8_t[gbaSaveSize];
            memset(gbaSave, 0xFF, gbaSaveSize * sizeof(uint8_t));
        }
    }
}

void Cartridge::directBoot()
{
    // Load the ROM header from file if needed
    if (ndsRomFile)
        loadRomSection(0, 0x170);

    // Extract some information about the initial ARM9 code from the header
    uint32_t offset9    = U8TO32(ndsRom, 0x20);
    uint32_t entryAddr9 = U8TO32(ndsRom, 0x24);
    uint32_t ramAddr9   = U8TO32(ndsRom, 0x28);
    uint32_t size9      = U8TO32(ndsRom, 0x2C);
    LOG("ARM9 code ROM offset:    0x%X\n", offset9);
    LOG("ARM9 code entry address: 0x%X\n", entryAddr9);
    LOG("ARM9 RAM address:        0x%X\n", ramAddr9);
    LOG("ARM9 code size:          0x%X\n", size9);

    // Extract some information about the initial ARM7 code from the header
    uint32_t offset7    = U8TO32(ndsRom, 0x30);
    uint32_t entryAddr7 = U8TO32(ndsRom, 0x34);
    uint32_t ramAddr7   = U8TO32(ndsRom, 0x38);
    uint32_t size7      = U8TO32(ndsRom, 0x3C);
    LOG("ARM7 code ROM offset:    0x%X\n", offset7);
    LOG("ARM7 code entry address: 0x%X\n", entryAddr7);
    LOG("ARM7 RAM address:        0x%X\n", ramAddr7);
    LOG("ARM7 code size:          0x%X\n", size7);

    // Load the ROM header into memory
    for (uint32_t i = 0; i < 0x170; i += 4)
        core->memory.write<uint32_t>(0, 0x27FFE00 + i, U8TO32(ndsRom, i));

    uint32_t offset;

    // Load the initial ARM9 code from file if needed
    if (ndsRomFile)
    {
        loadRomSection(offset9, size9);
        offset = 0;
    }
    else
    {
        offset = offset9;
    }

    // Load the initial ARM9 code into memory
    for (uint32_t i = 0; i < size9; i += 4)
    {
        if (ndsRomEncrypted && offset9 + i >= 0x4000 && offset9 + i < 0x4800)
        {
            if (offset9 + i < 0x4008)
            {
                // Overwrite the 'encryObj' string
                core->memory.write<uint32_t>(0, ramAddr9 + i, 0xE7FFDEFF);
            }
            else
            {
                // Decrypt the first 2KB of the secure area
                initKeycode(3);
                uint64_t data = decrypt64(U8TO64(ndsRom, (offset + i) & ~7));
                core->memory.write<uint32_t>(0, ramAddr9 + i, data >> (((offset + i) & 4) ? 32 : 0));
            }
        }
        else
        {
            core->memory.write<uint32_t>(0, ramAddr9 + i, U8TO32(ndsRom, offset + i));
        }
    }

    // Load the initial ARM7 code from file if needed
    if (ndsRomFile)
    {
        loadRomSection(offset7, size7);
        offset = 0;
    }
    else
    {
        offset = offset7;
    }

    // Load the initial ARM7 code into memory
    for (uint32_t i = 0; i < size7; i += 4)
    {
        if (ndsRomEncrypted && offset7 + i >= 0x4000 && offset7 + i < 0x4800)
        {
            if (offset7 + i < 0x4008)
            {
                // Overwrite the 'encryObj' string
                core->memory.write<uint32_t>(1, ramAddr7 + i, 0xE7FFDEFF);
            }
            else
            {
                // Decrypt the first 2KB of the secure area
                initKeycode(3);
                uint64_t data = decrypt64(U8TO64(ndsRom, (offset + i) & ~7));
                core->memory.write<uint32_t>(1, ramAddr7 + i, data >> (((offset + i) & 4) ? 32 : 0));
            }
        }
        else
        {
            core->memory.write<uint32_t>(1, ramAddr7 + i, U8TO32(ndsRom, offset + i));
        }
    }

    // Scan the initial ARM9 binary for a DLDI header and patch the driver if found
    for (int i = ramAddr9; i < ramAddr9 + size9; i += 0x40)
    {
        if (core->memory.read<uint32_t>(0, i) == 0xBF8DA5ED)
        {
            core->dldi.patchDriver(i);
            break;
        }
    }
}

void Cartridge::writeSave()
{
    // Update the NDS save file if necessary
    if (ndsSaveDirty)
    {
        FILE *ndsSaveFile = fopen(ndsSaveName.c_str(), "wb");
        if (ndsSaveFile)
        {
            if (ndsSaveSize > 0)
                fwrite(ndsSave, sizeof(uint8_t), ndsSaveSize, ndsSaveFile);
            fclose(ndsSaveFile);
            ndsSaveDirty = false;
        }
    }

    // Update the GBA save file if necessary
    if (gbaSaveDirty)
    {
        FILE *gbaSaveFile = fopen(gbaSaveName.c_str(), "wb");
        if (gbaSaveFile)
        {
            if (gbaSaveSize > 0)
                fwrite(gbaSave, sizeof(uint8_t), gbaSaveSize, gbaSaveFile);
            fclose(gbaSaveFile);
            gbaSaveDirty = false;
        }
    }
}

void Cartridge::trimRom(uint8_t **rom, int *romSize, std::string *romName)
{
    // Starting from the end, reduce the ROM size until a non-filler word is found
    int newSize;
    for (newSize = *romSize & ~3; newSize > 0; newSize -= 4)
    {
        if (U8TO32(*rom, newSize - 4) != 0xFFFFFFFF)
            break;
    }

    if (newSize < *romSize)
    {
        // Update the ROM in memory
        *romSize = newSize;
        uint8_t *newRom = new uint8_t[newSize];
        memcpy(newRom, *rom, newSize * sizeof(uint8_t));
        delete[] *rom;
        *rom = newRom;

        // Update the ROM file
        FILE *romFile = fopen(romName->c_str(), "wb");
        if (romFile)
        {
            if (newSize > 0)
                fwrite(*rom, sizeof(uint8_t), newSize, romFile);
            fclose(romFile);
        }
    }
}

void Cartridge::resizeSave(int newSize, uint8_t **save, int *saveSize, bool *saveDirty)
{
    // Resize the save
    if (newSize > 0)
    {
        uint8_t *newSave = new uint8_t[newSize];

        if (*saveSize < newSize) // New save is larger
        {
            // Copy all of the old save and fill the rest with 0xFF
            if (*saveSize < 0) *saveSize = 0;
            memcpy(newSave, *save, *saveSize * sizeof(uint8_t));
            memset(&newSave[*saveSize], 0xFF, (newSize - *saveSize) * sizeof(uint8_t));
        }
        else // New save is smaller
        {
            // Copy as much of the old save as possible
            memcpy(newSave, *save, newSize * sizeof(uint8_t));
        }

        delete[] *save;
        *save = newSave;
    }

    *saveSize = newSize;
    *saveDirty = true;
}

void Cartridge::resizeSave(int newSize, bool dirty)
{
    // Resize the save
    if (newSize > 0)
    {
        uint8_t *newSave = new uint8_t[newSize];

        if (ndsSaveSize < newSize) // New save is larger
        {
            // Copy all of the old save and fill the rest with 0xFF
            if (ndsSaveSize < 0) ndsSaveSize = 0;
            memcpy(newSave, ndsSave, ndsSaveSize * sizeof(uint8_t));
            memset(&newSave[ndsSaveSize], 0xFF, (newSize - ndsSaveSize) * sizeof(uint8_t));
        }
        else // New save is smaller
        {
            // Copy as much of the old save as possible
            memcpy(newSave, ndsSave, newSize * sizeof(uint8_t));
        }

        delete[] ndsSave;
        ndsSave = newSave;
    }

    ndsSaveSize = newSize;
    if (dirty)
        ndsSaveDirty = true;
}

uint8_t Cartridge::gbaEepromRead()
{
    if (gbaSaveSize == -1)
    {
        // Detect the save size based on how many command bits were sent before reading
        if (gbaEepromCount == 9)
        {
            gbaSaveSize = 0x200;
            LOG("Detected EEPROM 0.5KB save type\n");
        }
        else
        {
            gbaSaveSize = 0x2000;
            LOG("Detected EEPROM 8KB save type\n");
        }

        // Create a new save
        gbaSave = new uint8_t[gbaSaveSize];
        memset(gbaSave, 0xFF, gbaSaveSize * sizeof(uint8_t));
    }

    // EEPROM 0.5KB uses 8-bit commands, and EEPROM 8KB uses 16-bit commands
    int length = (gbaSaveSize == 0x200) ? 8 : 16;

    if (((gbaEepromCmd & 0xC000) >> 14) == 0x3 && gbaEepromCount >= length + 1) // Read
    {
        if (++gbaEepromCount >= length + 6)
        {
            // Read the data bits, MSB first
            int bit = 63 - (gbaEepromCount - (length + 6));
            uint16_t addr = (gbaSaveSize == 0x200) ? ((gbaEepromCmd & 0x3F00) >> 8) : (gbaEepromCmd & 0x03FF);
            uint8_t value = (gbaSave[addr * 8 + bit / 8] & BIT(bit % 8)) >> (bit % 8);

            // Reset the transfer at the end
            if (gbaEepromCount >= length + 69)
            {
                gbaEepromCount = 0;
                gbaEepromCmd = 0;
                gbaEepromData = 0;
            }

            return value;
        }
    }
    else if (gbaEepromDone)
    {
        // Signal that a write has finished
        return 1;
    }

    return 0;
}

void Cartridge::gbaEepromWrite(uint8_t value)
{
    gbaEepromDone = false;

    // EEPROM 0.5KB uses 8-bit commands, and EEPROM 8KB uses 16-bit commands
    int length = (gbaSaveSize == 0x200) ? 8 : 16;

    if (gbaEepromCount < length)
    {
        // Get the command bits
        gbaEepromCmd |= (value & BIT(0)) << (16 - ++gbaEepromCount);
    }
    else if (((gbaEepromCmd & 0xC000) >> 14) == 0x3) // Read
    {
        // Accept the last bit to finish the read command
        if (gbaEepromCount < length + 1)
            gbaEepromCount++;
    }
    else if (((gbaEepromCmd & 0xC000) >> 14) == 0x2) // Write
    {
        // Get the data bits, MSB first
        if (++gbaEepromCount <= length + 64)
            gbaEepromData |= (uint64_t)(value & BIT(0)) << (length + 64 - gbaEepromCount);

        if (gbaEepromCount >= length + 65)
        {
            // Games will probably read first, so the save size can be detected from that
            // If something decides to write first, it becomes a lot harder to detect the size
            // In this case, just assume EEPROM 8KB and create an empty save
            if (gbaSaveSize == -1)
            {
                gbaSaveSize = 0x2000;
                LOG("Detected EEPROM 8KB save type\n");
                gbaSave = new uint8_t[gbaSaveSize];
                memset(gbaSave, 0xFF, gbaSaveSize * sizeof(uint8_t));
            }

            // Write the data after all the bits have been received
            uint16_t addr = (gbaSaveSize == 0x200) ? ((gbaEepromCmd & 0x3F00) >> 8) : (gbaEepromCmd & 0x03FF);
            for (unsigned int i = 0; i < 8; i++)
                gbaSave[addr * 8 + i] = gbaEepromData >> (i * 8);
            gbaSaveDirty = true;

            // Reset the transfer
            gbaEepromCount = 0;
            gbaEepromCmd = 0;
            gbaEepromData = 0;
            gbaEepromDone = true;
        }
    }
}

uint8_t Cartridge::gbaSramRead(uint32_t address)
{
    if (gbaSaveSize == 0x8000 && address < 0xE008000) // SRAM
    {
        // Read a single byte because the data bus is only 8 bits
        return gbaSave[address - 0xE000000];
    }
    else if ((gbaSaveSize == 0x10000 || gbaSaveSize == 0x20000) && address < 0xE010000) // FLASH
    {
        // Run a FLASH command
        if (gbaFlashCmd == 0x90 && address == 0xE000000)
        {
            // Read the chip manufacturer ID
            return 0xC2;
        }
        else if (gbaFlashCmd == 0x90 && address == 0xE000001)
        {
            // Read the chip device ID
            return (gbaSaveSize == 0x10000) ? 0x1C : 0x09;
        }
        else
        {
            // Read a single byte
            if (gbaBankSwap) address += 0x10000;
            return gbaSave[address - 0xE000000];
        }
    }

    return 0xFF;
}

void Cartridge::gbaSramWrite(uint32_t address, uint8_t value)
{
    if (gbaSaveSize == 0x8000 && address < 0xE008000) // SRAM
    {
        // Write a single byte because the data bus is only 8 bits
        gbaSave[address - 0xE000000] = value;
        gbaSaveDirty = true;
    }
    else if ((gbaSaveSize == 0x10000 || gbaSaveSize == 0x20000) && address < 0xE010000) // FLASH
    {
        // Run a FLASH command
        if (gbaFlashCmd == 0xA0)
        {
            // Write a single byte
            if (gbaBankSwap) address += 0x10000;
            gbaSave[address - 0xE000000] = value;
            gbaSaveDirty = true;
            gbaFlashCmd = 0xF0;
        }
        else if (gbaFlashErase && (address & ~0x000F000) == 0xE000000 && (value & 0xFF) == 0x30)
        {
            // Erase a sector
            if (gbaBankSwap) address += 0x10000;
            memset(&gbaSave[address - 0xE000000], 0xFF, 0x1000 * sizeof(uint8_t));
            gbaSaveDirty = true;
            gbaFlashErase = false;
        }
        else if (gbaSaveSize == 0x20000 && gbaFlashCmd == 0xB0 && address == 0xE000000)
        {
            // Swap the ROM banks on 128KB carts
            gbaBankSwap = value;
            gbaFlashCmd = 0xF0;
        }
        else if (address == 0xE005555)
        {
            // Write the FLASH command byte
            gbaFlashCmd = value;

            // Handle erase commands
            if (gbaFlashCmd == 0x80)
            {
                gbaFlashErase = true;
            }
            else if (gbaFlashCmd != 0xAA)
            {
                gbaFlashErase = false;
            }
            else if (gbaFlashErase && gbaFlashCmd == 0x10)
            {
                memset(gbaSave, 0xFF, gbaSaveSize * sizeof(uint8_t));
                gbaSaveDirty = true;
            }
        }
    }
}

uint64_t Cartridge::encrypt64(uint64_t value)
{
    // Encrypt a 64-bit value using the Blowfish algorithm
    // This is a translation of the pseudocode from GBATEK to C++

    uint32_t y = value;
    uint32_t x = value >> 32;

    for (int i = 0x00; i <= 0x0F; i++)
    {
        uint32_t z = encTable[i] ^ x;
        x = encTable[0x012 + ((z >> 24) & 0xFF)];
        x = encTable[0x112 + ((z >> 16) & 0xFF)] + x;
        x = encTable[0x212 + ((z >>  8) & 0xFF)] ^ x;
        x = encTable[0x312 + ((z >>  0) & 0xFF)] + x;
        x ^= y;
        y = z;
    }

    return ((uint64_t)(y ^ encTable[0x11]) << 32) | (x ^ encTable[0x10]);
}

uint64_t Cartridge::decrypt64(uint64_t value)
{
    // Decrypt a 64-bit value using the Blowfish algorithm
    // This is a translation of the pseudocode from GBATEK to C++

    uint32_t y = value;
    uint32_t x = value >> 32;

    for (int i = 0x11; i >= 0x02; i--)
    {
        uint32_t z = encTable[i] ^ x;
        x = encTable[0x012 + ((z >> 24) & 0xFF)];
        x = encTable[0x112 + ((z >> 16) & 0xFF)] + x;
        x = encTable[0x212 + ((z >>  8) & 0xFF)] ^ x;
        x = encTable[0x312 + ((z >>  0) & 0xFF)] + x;
        x ^= y;
        y = z;
    }

    return ((uint64_t)(y ^ encTable[0x00]) << 32) | (x ^ encTable[0x01]);
}

void Cartridge::initKeycode(int level)
{
    // Initialize the Blowfish encryption table
    // This is a translation of the pseudocode from GBATEK to C++

    for (int i = 0; i < 0x412; i++)
        encTable[i] = core->memory.read<uint32_t>(1, 0x30 + i * 4);

    encCode[0] = ndsRomCode;
    encCode[1] = ndsRomCode / 2;
    encCode[2] = ndsRomCode * 2;

    if (level >= 1) applyKeycode();
    if (level >= 2) applyKeycode();

    encCode[1] *= 2;
    encCode[2] /= 2;

    if (level >= 3) applyKeycode();
}

void Cartridge::applyKeycode()
{
    // Apply a keycode to the Blowfish encryption table
    // This is a translation of the pseudocode from GBATEK to C++
    
    uint64_t enc1 = encrypt64(((uint64_t)encCode[2] << 32) | encCode[1]);
    encCode[1] = enc1;
    encCode[2] = enc1 >> 32;

    uint64_t enc2 = encrypt64(((uint64_t)encCode[1] << 32) | encCode[0]);
    encCode[0] = enc2;
    encCode[1] = enc2 >> 32;

    for (int i = 0; i <= 0x11; i++)
    {
        uint32_t byteReverse = 0;
        for (int j = 0; j < 4; j++)
            byteReverse |= ((encCode[i % 2] >> (j * 8)) & 0xFF) << ((3 - j) * 8);

        encTable[i] ^= byteReverse;
    }

    uint64_t scratch = 0;

    for (int i = 0; i <= 0x410; i += 2)
    {
        scratch = encrypt64(scratch);
        encTable[i + 0] = scratch >> 32;
        encTable[i + 1] = scratch;
    }
}

void Cartridge::writeAuxSpiCnt(bool cpu, uint16_t mask, uint16_t value)
{
    // Write to one of the AUXSPICNT registers
    mask &= 0xE043;
    auxSpiCnt[cpu] = (auxSpiCnt[cpu] & ~mask) | (value & mask);
}

void Cartridge::writeRomCmdOutL(bool cpu, uint32_t mask, uint32_t value)
{
    // Write to one of the ROMCMDOUT registers (low)
    romCmdOut[cpu] = (romCmdOut[cpu] & ~((uint64_t)mask)) | (value & mask);
}

void Cartridge::writeRomCmdOutH(bool cpu, uint32_t mask, uint32_t value)
{
    // Write to one of the ROMCMDOUT registers (high)
    romCmdOut[cpu] = (romCmdOut[cpu] & ~((uint64_t)mask << 32)) | ((uint64_t)(value & mask) << 32);
}

void Cartridge::writeAuxSpiData(bool cpu, uint8_t value)
{
    // Do nothing if there is no save
    if (ndsSaveSize == 0) return;

    if (auxWriteCount[cpu] == 0)
    {
        // On the first write, set the command byte
        if (value == 0) return;
        auxCommand[cpu] = value;
        auxAddress[cpu] = 0;
        auxSpiData[cpu] = 0;
    }
    else
    {
        // Incredibly naive save type detection, based on commands that might be sent
        if (ndsSaveSize == -1)
        {
            switch (auxCommand[cpu])
            {
                case 0x0B: // EEPROM 0.5KB: Read from upper memory
                    LOG("Detected EEPROM 0.5KB save type\n");
                    resizeSave(0x200, false);
                    break;

                case 0x02: // EEPROM 64KB: Write to memory
                    LOG("Detected EEPROM 64KB save type\n");
                    resizeSave(0x10000, false);
                    break;

                case 0x0A: // FLASH 512KB: Page write
                    LOG("Detected FLASH 512KB save type\n");
                    resizeSave(0x80000, false);
                    break;

                default:
                    // Deselect chip
                    if (!(auxSpiCnt[cpu] & BIT(6)))
                        auxWriteCount[cpu] = 0;
                    return;
            }
        }


        switch (ndsSaveSize)
        {
            case 0x200: // EEPROM 0.5KB
            {
                switch (auxCommand[cpu])
                {
                    case 0x03: // Read from lower memory
                    {
                        if (auxWriteCount[cpu] < 2)
                        {
                            // On the second write, set the 1 byte address to read from
                            auxAddress[cpu] = value;
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 3+, read data from the save and send it back
                            auxSpiData[cpu] = (auxAddress[cpu] < 0x200) ? ndsSave[auxAddress[cpu]] : 0;
                            auxAddress[cpu]++;
                        }
                        break;
                    }

                    case 0x0B: // Read from upper memory
                    {
                        if (auxWriteCount[cpu] < 2)
                        {
                            // On the second write, set the 1 byte address to read from
                            auxAddress[cpu] = 0x100 + value;
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 3+, read data from the save and send it back
                            auxSpiData[cpu] = (auxAddress[cpu] < 0x200) ? ndsSave[auxAddress[cpu]] : 0;
                            auxAddress[cpu]++;
                        }
                        break;
                    }

                    case 0x02: // Write to lower memory
                    {
                        if (auxWriteCount[cpu] < 2)
                        {
                            // On the second write, set the 1 byte address to write to
                            auxAddress[cpu] = value;
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 3+, write data to the save
                            if (auxAddress[cpu] < 0x200)
                            {
                                ndsSave[auxAddress[cpu]] = value;
                                ndsSaveDirty = true;
                            }

                            auxAddress[cpu]++;
                            auxSpiData[cpu] = 0;
                        }
                        break;
                    }

                    case 0x0A: // Write to upper memory
                    {
                        if (auxWriteCount[cpu] < 2)
                        {
                            // On the second write, set the 1 byte address to write to
                            auxAddress[cpu] = 0x100 + value;
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 3+, write data to the save
                            if (auxAddress[cpu] < 0x200)
                            {
                                ndsSave[auxAddress[cpu]] = value;
                                ndsSaveDirty = true;
                            }

                            auxAddress[cpu]++;
                            auxSpiData[cpu] = 0;
                        }
                        break;
                    }

                    default:
                    {
                        LOG("Write to AUX SPI with unknown EEPROM 0.5KB command: 0x%X\n", auxCommand[cpu]);
                        auxSpiData[cpu] = 0;
                        break;
                    }
                }
                break;
            }

            case 0x2000: case 0x8000: case 0x10000: case 0x20000: // EEPROM 8KB, 64KB, 128KB; FRAM 32KB
            {
                switch (auxCommand[cpu])
                {
                    case 0x03: // Read from memory
                    {
                        if (auxWriteCount[cpu] < ((ndsSaveSize == 0x20000) ? 4 : 3))
                        {
                            // On writes 2-3, set the 2 byte address to read from (not EEPROM 128KB)
                            // EEPROM 128KB uses a 3 byte address, so it's set on writes 2-4
                            auxAddress[cpu] |= value << ((((ndsSaveSize == 0x20000) ? 3 : 2) - auxWriteCount[cpu]) * 8);
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 4+, read data from the save and send it back
                            auxSpiData[cpu] = (auxAddress[cpu] < ndsSaveSize) ? ndsSave[auxAddress[cpu]] : 0;
                            auxAddress[cpu]++;
                        }
                        break;
                    }

                    case 0x02: // Write to memory
                    {
                        if (auxWriteCount[cpu] < ((ndsSaveSize == 0x20000) ? 4 : 3))
                        {
                            // On writes 2-3, set the 2 byte address to write to (not EEPROM 128KB)
                            // EEPROM 128KB uses a 3 byte address, so it's set on writes 2-4
                            auxAddress[cpu] |= value << ((((ndsSaveSize == 0x20000) ? 3 : 2) - auxWriteCount[cpu]) * 8);
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 4+, write data to the save
                            if (auxAddress[cpu] < ndsSaveSize)
                            {
                                ndsSave[auxAddress[cpu]] = value;
                                ndsSaveDirty = true;
                            }

                            auxAddress[cpu]++;
                            auxSpiData[cpu] = 0;
                        }
                        break;
                    }

                    default:
                    {
                        LOG("Write to AUX SPI with unknown EEPROM/FRAM command: 0x%X\n", auxCommand[cpu]);
                        auxSpiData[cpu] = 0;
                        break;
                    }
                }
                break;
            }

            case 0x40000: case 0x80000: case 0x100000: case 0x800000: // FLASH 256KB, 512KB, 1024KB, 8192KB
            {
                switch (auxCommand[cpu])
                {
                    case 0x03: // Read data bytes
                    {
                        if (auxWriteCount[cpu] < 4)
                        {
                            // On writes 2-4, set the 3 byte address to read from
                            auxAddress[cpu] |= value << ((3 - auxWriteCount[cpu]) * 8);
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 5+, read data from the save and send it back
                            auxSpiData[cpu] = (auxAddress[cpu] < ndsSaveSize) ? ndsSave[auxAddress[cpu]] : 0;
                            auxAddress[cpu]++;
                        }
                        break;
                    }

                    case 0x0A: // Page write
                    {
                        if (auxWriteCount[cpu] < 4)
                        {
                            // On writes 2-4, set the 3 byte address to write to
                            auxAddress[cpu] |= value << ((3 - auxWriteCount[cpu]) * 8);
                            auxSpiData[cpu] = 0;
                        }
                        else
                        {
                            // On writes 5+, write data to the save
                            if (auxAddress[cpu] < ndsSaveSize)
                            {
                                ndsSave[auxAddress[cpu]] = value;
                                ndsSaveDirty = true;
                            }

                            auxAddress[cpu]++;
                            auxSpiData[cpu] = 0;
                        }
                        break;
                    }

                    case 0x08: // IR-related
                    {
                        // If a gamecode starts with 'I', the game has an infrared port in its cartridge
                        // This shares the same SPI as FLASH memory
                        // Some games check this command as an anti-piracy measure
                        auxSpiData[cpu] = ((ndsRomCode & 0xFF) == 'I') ? 0xAA : 0;
                        break;
                    }

                    default:
                    {
                        LOG("Write to AUX SPI with unknown FLASH command: 0x%X\n", auxCommand[cpu]);
                        auxSpiData[cpu] = 0;
                        break;
                    }
                }
                break;
            }

            default:
            {
                LOG("Write to AUX SPI with unknown save size: 0x%X\n", ndsSaveSize);
                break;
            }
        }
    }

    // Keep track of the write count
    if (auxSpiCnt[cpu] & BIT(6)) // Keep chip selected
        auxWriteCount[cpu]++;
    else // Deselect chip
        auxWriteCount[cpu] = 0;
}

void Cartridge::writeRomCtrl(bool cpu, uint32_t mask, uint32_t value)
{
    bool transfer = false;

    // Set the release reset bit, but never clear it
    romCtrl[cpu] |= (value & BIT(29));

    // Start a transfer if the start bit changes from 0 to 1
    if (!(romCtrl[cpu] & BIT(31)) && (value & BIT(31)))
        transfer = true;

    // Write to one of the ROMCTRL registers
    mask &= 0xDF7F7FFF;
    romCtrl[cpu] = (romCtrl[cpu] & ~mask) | (value & mask);

    if (!transfer) return;

    // Determine the size of the block to transfer
    uint8_t size = (romCtrl[cpu] & 0x07000000) >> 24;
    switch (size)
    {
        case 0:  blockSize[cpu] = 0;             break;
        case 7:  blockSize[cpu] = 4;             break;
        default: blockSize[cpu] = 0x100 << size; break;
    }

    // Reverse the byte order of the ROM command
    uint64_t command = 0;
    for (int i = 0; i < 8; i++)
        command |= ((romCmdOut[cpu] >> (i * 8)) & 0xFF) << ((7 - i) * 8);

    // Decrypt the ROM command if encryption is enabled
    if (encrypted[cpu])
    {
        initKeycode(2);
        command = decrypt64(command);
    }

    ndsCmdMode = CMD_NONE;

    // Interpret the ROM command
    if (ndsRom)
    {
        if (command == 0x0000000000000000) // Get header
        {
            ndsCmdMode = CMD_HEADER;

            // Load the header from file if needed
            if (ndsRomFile)
                loadRomSection(0, blockSize[cpu]);
        }
        else if (command == 0x9000000000000000 || (command >> 60) == 0x1 || command == 0xB800000000000000) // Get chip ID
        {
            ndsCmdMode = CMD_CHIP;
        }
        else if ((command >> 56) == 0x3C) // Activate KEY1 encryption mode
        {
            // Initialize KEY1 encryption
            encrypted[cpu] = true;
        }
        else if ((command >> 60) == 0x2) // Get secure area
        {
            ndsCmdMode = CMD_SECURE;
            romAddrReal[cpu] = ((command & 0x0FFFF00000000000) >> 44) * 0x1000;

            // Load the secure area block from file if needed
            if (ndsRomFile)
            {
                loadRomSection(romAddrReal[cpu], blockSize[cpu]);
                romAddrVirt[cpu] = 0;
            }
            else
            {
                romAddrVirt[cpu] = romAddrReal[cpu];
            }
        }
        else if (((command >> 56) & 0xF0) == 0xA0) // Enter main data mode
        {
            // Disable KEY1 encryption
            // On hardware, this is where KEY2 encryption would start
            encrypted[cpu] = false;
        }
        else if ((command & 0xFF00000000FFFFFF) == 0xB700000000000000) // Get data
        {
            ndsCmdMode = CMD_DATA;
            romAddrReal[cpu] = (command & 0x00FFFFFFFF000000) >> 24;

            // Load the ROM data from file if needed
            if (ndsRomFile)
            {
                if (romAddrReal[cpu] < 0x8000)
                {
                    // Workaround for address redirection when loading from file
                    loadRomSection(0, 0x8200 + blockSize[cpu]);
                    romAddrVirt[cpu] = romAddrReal[cpu];
                }
                else
                {
                    loadRomSection(romAddrReal[cpu], blockSize[cpu]);
                    romAddrVirt[cpu] = 0;
                }
            }
            else
            {
                romAddrVirt[cpu] = romAddrReal[cpu];
            }
        }
        else if (command != 0x9F00000000000000) // Unknown (not dummy)
        {
            LOG("ROM transfer with unknown command: 0x%llX\n", command);
        }
    }

    if (blockSize[cpu] == 0)
    {
        // End the transfer right away if the block size is 0
        romCtrl[cpu] &= ~BIT(23); // Word not ready
        romCtrl[cpu] &= ~BIT(31); // Block ready

        // Trigger a block ready IRQ if enabled
        if (auxSpiCnt[cpu] & BIT(14))
            core->interpreter[cpu].sendInterrupt(19);
    }
    else
    {
        // Indicate that a word is ready
        romCtrl[cpu] |= BIT(23);

        // Trigger DS cartridge DMA transfers
        core->dma[cpu].trigger((cpu == 0) ? 5 : 2);

        readCount[cpu] = 0;
    }
}

uint32_t Cartridge::readRomDataIn(bool cpu)
{
    // Don't transfer if the word ready bit isn't set
    if (!(romCtrl[cpu] & BIT(23)))
        return 0;

    // Increment the read counter
    if ((readCount[cpu] += 4) == blockSize[cpu])
    {
        // End the transfer when the block size has been reached
        romCtrl[cpu] &= ~BIT(23); // Word not ready
        romCtrl[cpu] &= ~BIT(31); // Block ready

        // Trigger a block ready IRQ if enabled
        if (auxSpiCnt[cpu] & BIT(14))
            core->interpreter[cpu].sendInterrupt(19);
    }
    else
    {
        // Trigger DS cartridge DMA transfers until the block size is reached
        core->dma[cpu].trigger((cpu == 0) ? 5 : 2);
    }

    // Return a value from the cart depending on the current command
    switch (ndsCmdMode)
    {
        case CMD_HEADER:
        {
            // Read the ROM header, repeated every 0x1000 bytes
            return U8TO32(ndsRom, (readCount[cpu] - 4) & 0xFFF);
        }

        case CMD_CHIP:
        {
            // Read the chip ID, repeated every 4 bytes
            // ROM dumps don't provide a chip ID, so use a fake one
            return 0x00001FC2;
        }

        case CMD_SECURE:
        {
            // Encrypt the first 2KB of the secure area
            if (!ndsRomEncrypted && romAddrReal[cpu] == 0x4000 && readCount[cpu] <= 0x800)
            {
                // Supply the 'encryObj' string for the first 8 bytes (overwritten during decryption)
                uint64_t data = (readCount[cpu] <= 8) ? 0x6A624F7972636E65 :
                    U8TO64(ndsRom, (romAddrVirt[cpu] + readCount[cpu] - 4) & ~7);

                // Encrypt the data
                initKeycode(3);
                data = encrypt64(data);

                // Double-encrypt the 'encryObj' string
                if (readCount[cpu] <= 8)
                {
                    initKeycode(2);
                    data = encrypt64(data);
                }

                return data >> (((romAddrReal[cpu] + readCount[cpu]) & 4) ? 0 : 32);
            }

            // Read data from the selected secure area block
            return U8TO32(ndsRom, romAddrVirt[cpu] + readCount[cpu] - 4);
        }

        case CMD_DATA:
        {
            // Read ROM data from the given address
            // This command can't read the first 32KB of a ROM, so it redirects the address
            // Some games verify that the first 32KB are unreadable as an anti-piracy measure
            uint32_t address = romAddrVirt[cpu] + readCount[cpu] - 4;
            if (romAddrReal[cpu] + readCount[cpu] <= 0x8000) address = 0x8000 + (address & 0x1FF);
            if (address < ndsRomSize) return U8TO32(ndsRom, address);
        }
    }

    // Default to endless 0xFFs if there's no actual data to read
    return 0xFFFFFFFF;
}
