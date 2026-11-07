ENTRY(_start)
SECTIONS {
    . = BASE;
    .start : { *(.start) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .expected ALIGN(4) : { KEEP(*(.expected)) }
    __end = .;
    .bss ALIGN(4) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
    __end__ = .;
}
