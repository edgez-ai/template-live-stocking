#if defined(MORSE_HALOW_FIRMWARE_PATH) && defined(MORSE_HALOW_BCF_PATH)

#define ASM_INCBIN(path) ".incbin \"" path "\"\n"

__asm__(
	".section .rodata.morse_firmware,\"a\",%progbits\n"
	".balign 4\n"
	".global firmware_binary_start\n"
	".global firmware_binary_end\n"
	"firmware_binary_start:\n"
	ASM_INCBIN(MORSE_HALOW_FIRMWARE_PATH)
	".balign 4\n"
	"firmware_binary_end:\n"
	".section .rodata.morse_bcf,\"a\",%progbits\n"
	".balign 4\n"
	".global bcf_binary_start\n"
	".global bcf_binary_end\n"
	"bcf_binary_start:\n"
	ASM_INCBIN(MORSE_HALOW_BCF_PATH)
	".balign 4\n"
	"bcf_binary_end:\n"
);

#endif
