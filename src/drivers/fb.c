#include <drivers/fb.h>
#include <common/boot.h>
#include <flanterm/backends/fb.h>
framebuffer_t early_fb;
framebuffer_t *global_fb = NULL;

void framebuffer_early_init() {
	uint32_t default_bg = 0x313647;
	uint32_t default_fg = 0xFFF8D4;
	early_fb.width = TitanBootInfo.framebuffer.width;
	early_fb.height = TitanBootInfo.framebuffer.height;
	early_fb.bpp = TitanBootInfo.framebuffer.bpp;
	early_fb.lfb = CAST_AS(void*, TitanBootInfo.framebuffer.addr);
	early_fb.ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        TitanBootInfo.framebuffer.addr, TitanBootInfo.framebuffer.width, TitanBootInfo.framebuffer.height, TitanBootInfo.framebuffer.pitch,
        TitanBootInfo.framebuffer.red_mask, TitanBootInfo.framebuffer.red_shift,
        TitanBootInfo.framebuffer.green_mask, TitanBootInfo.framebuffer.green_shift,
        TitanBootInfo.framebuffer.blue_mask, TitanBootInfo.framebuffer.blue_shift,
        NULL,
        NULL, NULL,
        &default_bg, &default_fg,
        NULL, NULL,
        NULL, 0, 0, 1,
        0, 0,
        0,0
	);
	global_fb = &early_fb; // So we can access global fb as a pointer, not necessary.
}