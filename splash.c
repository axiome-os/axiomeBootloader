/* axboot boot splash: logo + rounded progress bar over the GOP scanout.

   The framebuffer is written directly (not through a back-buffer): the
   loader has no compositor and this runs before ExitBootServices, so a
   WC store burst plus SFENCE is enough.  The renderer itself is shared
   with the kernel via include/bootsplash.h. */

#include "bootloader.h"
#include "bootsplash.h"

void splash_surface(struct boot_ctx *ctx, struct splash_surface *s)
{
    s->pixels = (uint8_t *)(UINTN)ctx->fb_addr;
    s->width = ctx->fb_width;
    s->height = ctx->fb_height;
    s->pitch = ctx->fb_pitch;
}

void splash_show(struct boot_ctx *ctx)
{
    struct splash_surface s;

    if (!ctx->fb_valid || !ctx->splash_enabled)
        return;

    ctx->splash_progress = 30; /* loader has started */
    splash_surface(ctx, &s);
    splash_render(&s, ctx->splash_progress, 1);
    __asm__ volatile("sfence" ::: "memory");
}

void splash_set_progress(struct boot_ctx *ctx, UINT32 permille)
{
    struct splash_surface s;

    if (!ctx->fb_valid || !ctx->splash_enabled || permille > 1000)
        return;
    if (permille <= ctx->splash_progress)
        return;

    ctx->splash_progress = permille;
    splash_surface(ctx, &s);
    splash_render(&s, ctx->splash_progress, 0);
    __asm__ volatile("sfence" ::: "memory");
}
