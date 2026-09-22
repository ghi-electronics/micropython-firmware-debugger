/*
    GNU linker script for STM32C071xB (128 KB Flash, 24 KB RAM).

    Flash layout:
      0x08000000 .. 0x0801D7FF   FLASH  (118 KB) -- firmware
      0x0801D800 .. 0x0801FFFF   USER   ( 10 KB) -- user .mpy bundle
                                                    (uploaded at runtime via
                                                    the debug channel, loaded
                                                    by mp_raw_code_load_mem())

    RAM layout (24 KB):
      _sbss     ->  _ebss                 static .bss (~8 KB now that
                                          s_mpy_upload_buf lives in its own
                                          section, not in .bss).
      _ebss     ->  _upload_buf_start     initial Python GC heap (~4 KB).
      _upload_buf_start
                ->  _upload_buf_end       reclaimable 10 KB upload buffer.
                                          Used by mpy_boot.c during the
                                          loader window; handed to
                                          MicroPython's GC via gc_add() once
                                          the window closes, so real user
                                          code sees ~14 KB of Python heap.
      _sstack   ->  _estack               2 KB stack.

    The upload buffer and initial heap are physically adjacent, so after
    the gc_add() extension the two regions form one contiguous GC arena --
    avoids fragmentation that would otherwise plague large allocations.

    This script is self-contained (does not INCLUDE common_basic.ld) so
    that the .upload_buf section can be inserted between .heap and .stack
    without forking the shared common_bss_heap_stack.ld fragment.
*/

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 118K
    USER (r)        : ORIGIN = 0x0801D800, LENGTH = 10K
    RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 24K
}

/* Aliases used by the shared common_isr.ld / common_text.ld fragments. */
REGION_ALIAS("FLASH_ISR", FLASH);
REGION_ALIAS("FLASH_COMMON", FLASH);

/* Where the user .mpy bundle lives in flash.  Consumed by mpy_boot.c. */
_user_mpy_start = ORIGIN(USER);
_user_mpy_end   = ORIGIN(USER) + LENGTH(USER);
_user_mpy_size  = LENGTH(USER);

/* Link-time floors -- if either doesn't fit, the link fails cleanly. */
_minimum_stack_size = 1536;
_minimum_heap_size = 4K;

_ram_start = ORIGIN(RAM);
_ram_end = ORIGIN(RAM) + LENGTH(RAM);

/* Stack at the very top of RAM, full-descending, 8-byte aligned per EABI. */
_estack = ORIGIN(RAM) + LENGTH(RAM) - _estack_reserve;
_sstack = _estack - 2K;

SECTIONS
{
    INCLUDE common_isr.ld
    INCLUDE common_text.ld
    INCLUDE common_extratext_data_in_flash.ld

    /* Empty gc_blocks_table: the stm32 port's main.c walks this table under
     * MICROPY_GC_SPLIT_HEAP=1 and calls gc_add() for each {addr,size} entry.
     * We don't want that path to touch the upload buffer, because it runs
     * BEFORE our MICROPY_BOARD_RUN_MAIN_PY hook -- GC would allocate over
     * the buffer while the loader is still writing to it during upload.
     * Providing an empty table (zero entries) keeps the linker happy and
     * makes main.c's loop a no-op; we manually gc_add() the buffer later
     * from mpy_boot.c, once the loader window has closed. */
    .gc.blocks.table :
    {
        . = ALIGN(4);
        _gc_blocks_table_start = .;
        _gc_blocks_table_end = .;
    } > FLASH

    /* Zeroed-out data.  s_mpy_upload_buf is deliberately NOT here -- see
     * the .upload_buf section below. */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    /* Reserve at least _minimum_heap_size for the initial Python heap.
     * Actual heap extents are _heap_start..._heap_end below. */
    .heap :
    {
        . = ALIGN(4);
        . = . + _minimum_heap_size;
        . = ALIGN(4);
    } >RAM

    /* 10 KB upload buffer used by mpy_boot.c's loader window.  NOLOAD so
     * the CRT doesn't try to zero it (mpy_try_upload writes to it before
     * reading, and gc_add() calls gc_setup_area() which initializes the
     * region's metadata).  s_mpy_upload_buf in mpy_boot.c lands here via
     *   __attribute__((section(".upload_buf")))
     */
    .upload_buf (NOLOAD) :
    {
        . = ALIGN(8);
        _upload_buf_start = .;
        KEEP(*(.upload_buf))
        . = ALIGN(8);
        _upload_buf_end = .;
    } >RAM

    /* Reserve stack min so the link fails if RAM is over-committed. */
    .stack :
    {
        . = ALIGN(4);
        . = . + _minimum_stack_size;
        . = ALIGN(4);
    } >RAM
}

/* _bl_state symbol is used by MICROPY_HW_ENTER_BOOTLOADER_VIA_RESET. */
_bl_state = _estack;

/* Initial GC heap: from _ebss up to the start of the upload buffer.
 * Once the loader window closes, mpy_boot.c calls
 *   gc_add(_upload_buf_start, _upload_buf_end)
 * to hand those 10 KB to the GC as a second, adjacent heap area. */
_heap_start = _ebss;
_heap_end = _upload_buf_start;
