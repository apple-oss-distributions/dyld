

asm("\n .globl _abs_value" \
    "\n _abs_value = 0xF000000000000000");

asm("\n .globl _unserializable_abs_value" \
    "\n _unserializable_abs_value = 0x80000004FFFFFFFF");
