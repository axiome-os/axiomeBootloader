/* Debug-mode wait window.
 *
 * If the user holds SHIFT or presses 'P' during the ~3 second window,
 * debug=1 is added to the kernel command line.  The window shows a
 * progress bar so the user knows when to press a key. */

#include "bootloader.h"
#include "bootsplash.h"
#include <efi.h>

/* SimpleTextInputExProtocol GUID from gnu-efi headers. */
static EFI_GUID g_simple_text_input_ex_guid = {
    0xdd9e7534, 0x7762, 0x4698,
    {0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa}
};

#define EFI_SHIFT_STATE_VALID        0x80000000
#define EFI_RIGHT_SHIFT_PRESSED      0x00000001
#define EFI_LEFT_SHIFT_PRESSED       0x00000002

/* Check if a key event from SimpleTextInputExProtocol indicates SHIFT */
static int is_shift_key(EFI_KEY_DATA *key)
{
    UINT32 shift = key->KeyState.KeyShiftState;
    return (shift & EFI_SHIFT_STATE_VALID) &&
           ((shift & EFI_LEFT_SHIFT_PRESSED) || (shift & EFI_RIGHT_SHIFT_PRESSED));
}

/* Check if a key event indicates 'P' key (uppercase or lowercase) */
static int is_p_key(EFI_KEY_DATA *key)
{
    return (key->Key.UnicodeChar == 'p' || key->Key.UnicodeChar == 'P');
}

/* Check if a standard key event indicates 'P' key */
static int is_p_key_std(EFI_INPUT_KEY *key)
{
    return (key->UnicodeChar == 'p' || key->UnicodeChar == 'P');
}

/* Create a timer event that fires after `ms` milliseconds. */
static EFI_EVENT create_timer(EFI_BOOT_SERVICES *bs, UINTN ms)
{
    EFI_EVENT timer;
    EFI_STATUS status = bs->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer);
    if (EFI_ERROR(status))
        return NULL;
    status = bs->SetTimer(timer, TimerRelative, (UINT64)ms * 10000ULL);
    if (EFI_ERROR(status)) {
        bs->CloseEvent(timer);
        return NULL;
    }
    return timer;
}

/* Wait up to `total_ms` milliseconds for SHIFT or 'P' key.
   Returns 1 if trigger key was pressed, 0 on timeout. */
int splash_wait_for_shift(struct boot_ctx *ctx)
{
    if (!ctx->fb_valid || !ctx->splash_enabled)
        return 0;

    bl_print("axboot: hold SHIFT or press P for debug mode (3s)...\n");

    struct splash_surface s;
    splash_surface(ctx, &s);

    const UINTN total_ms = 3000;
    const UINTN poll_ms  = 50;

    /* Open SimpleTextInputExProtocol for shift state detection */
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = NULL;
    EFI_STATUS status = ctx->bs->OpenProtocol(ctx->st->ConsoleInHandle,
                                              &g_simple_text_input_ex_guid,
                                              (void **)&ex, ctx->image, NULL,
                                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    int have_ex = !EFI_ERROR(status) && ex != NULL;

    /* Create timer event for timeout */
    EFI_EVENT timer = create_timer(ctx->bs, total_ms);
    EFI_EVENT wait_event = ctx->st->ConIn->WaitForKey;
    EFI_EVENT events[2] = { wait_event, timer };
    int key_pressed = 0;

    if (!timer) {
        bl_print("axboot: failed to create timer, using polling\n");
    }

    while (1) {
        UINTN index;
        status = ctx->bs->WaitForEvent(timer ? 2 : 1, events, &index);
        if (EFI_ERROR(status)) {
            bl_print("axboot: WaitForEvent error\n");
            break;
        }

        if (index == 1) {
            /* Timer fired - timeout */
            bl_print("axboot: timeout\n");
            break;
        }

        /* index == 0: key event signaled */
        if (have_ex) {
            EFI_KEY_DATA key;
            status = ex->ReadKeyStrokeEx(ex, &key);
            if (!EFI_ERROR(status)) {
                bl_print("axboot: EX key: shift="); bl_print_hex(key.KeyState.KeyShiftState);
                bl_print(" uc="); bl_print_hex(key.Key.UnicodeChar); bl_print("\n");
                if (is_shift_key(&key) || is_p_key(&key)) {
                    key_pressed = 1;
                    break;
                }
            }
        } else {
            EFI_INPUT_KEY key;
            status = ctx->st->ConIn->ReadKeyStroke(ctx->st->ConIn, &key);
            if (!EFI_ERROR(status)) {
                bl_print("axboot: STD key: sc="); bl_print_hex(key.ScanCode);
                bl_print(" uc="); bl_print_hex(key.UnicodeChar); bl_print("\n");
                if (is_p_key_std(&key)) {
                    key_pressed = 1;
                    break;
                }
            }
        }
    }

    if (timer)
        ctx->bs->CloseEvent(timer);
    if (have_ex)
        ctx->bs->CloseProtocol(ctx->st->ConsoleInHandle,
                               &g_simple_text_input_ex_guid,
                               ctx->image, NULL);

    /* Progress bar animation during wait (also handles case where WaitForEvent doesn't work) */
    UINTN waited = 0;
    while (waited < total_ms && !key_pressed) {
        if (have_ex) {
            EFI_KEY_DATA key;
            status = ex->ReadKeyStrokeEx(ex, &key);
            if (status == EFI_NOT_READY) {
                /* No key */
            } else if (!EFI_ERROR(status)) {
                if (is_shift_key(&key) || is_p_key(&key)) {
                    key_pressed = 1;
                    break;
                }
            }
        } else {
            EFI_INPUT_KEY key;
            status = ctx->st->ConIn->ReadKeyStroke(ctx->st->ConIn, &key);
            if (status == EFI_NOT_READY) {
                /* No key */
            } else if (!EFI_ERROR(status)) {
                if (is_p_key_std(&key)) {
                    key_pressed = 1;
                    break;
                }
            }
        }

        ctx->splash_progress += 2;
        if (ctx->splash_progress > 250)
            ctx->splash_progress = 250;
        splash_render(&s, ctx->splash_progress, 0);
        __asm__ volatile("sfence" ::: "memory");

        ctx->bs->Stall(poll_ms * 1000);
        waited += poll_ms;
    }

    if (key_pressed)
        bl_print("axboot: trigger key pressed\n");
    else
        bl_print("axboot: no trigger key pressed\n");

    return key_pressed;
}