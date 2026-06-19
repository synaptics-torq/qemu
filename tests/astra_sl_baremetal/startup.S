.section .text.boot
.global _start
.type _start, %function
.extern irq_handler

.balign 2048
.global vector_table
vector_table:
    /* Current EL with SP0 */
    b .
    .balign 128
    b irq_entry
    .balign 128
    b .
    .balign 128
    b .

    /* Current EL with SPx */
    .balign 128
    b .
    .balign 128
    b irq_entry
    .balign 128
    b .
    .balign 128
    b .

    /* Lower EL using AArch64 */
    .balign 128
    b .
    .balign 128
    b irq_entry
    .balign 128
    b .
    .balign 128
    b .

    /* Lower EL using AArch32 */
    .balign 128
    b .
    .balign 128
    b irq_entry
    .balign 128
    b .
    .balign 128
    b .

irq_entry:
    sub sp, sp, #256
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30, [sp, #16 * 15]

    bl irq_handler

    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ldp x4, x5, [sp, #16 * 2]
    ldp x6, x7, [sp, #16 * 3]
    ldp x8, x9, [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30, [sp, #16 * 15]
    add sp, sp, #256
    eret

_start:
    ldr x0, =__stack_top
    mov sp, x0

    /* EL3-only startup path. */
    mrs x1, CurrentEL
    cmp x1, #0xc
    b.ne unexpected_el

    mrs x2, scr_el3
    orr x2, x2, #(1 << 1)   /* SCR_EL3.IRQ=1: route IRQ to EL3 */
    msr scr_el3, x2
    ldr x2, =vector_table
    msr vbar_el3, x2

    isb
    bl main

unexpected_el:
    wfe
    b unexpected_el
