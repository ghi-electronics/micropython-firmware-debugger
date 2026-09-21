/*
    GNU linker script for STM32C071xB (128 KB Flash, 24 KB RAM)

    Flash layout:
      0x08000000 .. 0x0801D7FF   FLASH  (118 KB) -- firmware (vector table + code + data)
      0x0801D800 .. 0x0801FFFF   USER   ( 10 KB) -- reserved for user .mpy bytecode

    The USER region is uploaded to at runtime via the debug channel (Scope B),
    then loaded by MicroPython's mp_raw_code_load_mem() at boot. Nothing is
    placed there at link time. The size is 5 flash pages (2 KB each on
    STM32C0), page-aligned so erase-during-upload is clean.
*/

/* Specify the memory areas */
MEMORY
{
    FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 118K
    USER (r)        : ORIGIN = 0x0801D800, LENGTH = 10K
    RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 24K
}

/* Symbols the firmware can reference to know where the user .mpy blob lives. */
_user_mpy_start = ORIGIN(USER);
_user_mpy_end   = ORIGIN(USER) + LENGTH(USER);
_user_mpy_size  = LENGTH(USER);

/* produce a link error if there is not this amount of RAM for these sections */
_minimum_stack_size = 2K;
_minimum_heap_size = 4K;

_ram_start = ORIGIN(RAM);
_ram_end = ORIGIN(RAM) + LENGTH(RAM);

/* Define the top end of the stack.  The stack is full descending so begins just
   above last byte of RAM.  Note that EABI requires the stack to be 8-byte
   aligned for a call. */
_estack = ORIGIN(RAM) + LENGTH(RAM) - _estack_reserve;
_sstack = _estack - 2K;

/* RAM extents for the main heap */
_heap_start = _ebss;
_heap_end = _sstack;
