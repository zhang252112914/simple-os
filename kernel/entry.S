        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text
.global _entry
_entry:
        # set up a stack for C.
        la sp, stack_top
        li a0, 1024*4
        add sp, sp, a0
        # jump to start() in start.c
        call start
spin:
        j spin
