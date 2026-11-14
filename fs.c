/* Read files from the ESP, using only UEFI protocols (SimpleFileSystem).
   No storage drivers here.
   Strategy: enumerate every SimpleFileSystem handle and try the requested
   path on each volume; the first hit wins. This avoids any dependence on
   LoadedImage/DevicePath plumbing. */

#include "bootloader.h"

static EFI_STATUS
try_volume(struct boot_ctx *ctx, EFI_HANDLE h, const CHAR16 *name,
           UINT8 **out, UINTN *out_size)
{
    EFI_STATUS status;
    EFI_GUID fs_guid = FileSystemProtocol;
    EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE *root = NULL;
    EFI_FILE *file = NULL;
    EFI_FILE_INFO *fi = NULL;
    UINTN fi_size = 0;
    UINTN read_size;
    UINT8 *buf;

    status = ctx->bs->OpenProtocol(h, &fs_guid, (void **)&fs,
                                   ctx->image, NULL,
                                   EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status) || !fs)
        return EFI_NOT_FOUND;

    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || !root)
        return EFI_NOT_FOUND;

    status = root->Open(root, &file, (CHAR16 *)name,
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file)
    {
        root->Close(root);
        return EFI_NOT_FOUND;
    }

    /* Query the file size first. */
    status = file->GetInfo(file, &fi_guid, &fi_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL)
    {
        file->Close(file);
        root->Close(root);
        return EFI_DEVICE_ERROR;
    }
    status = ctx->bs->AllocatePool(EfiLoaderData, fi_size, (void **)&fi);
    if (EFI_ERROR(status))
    {
        file->Close(file);
        root->Close(root);
        return status;
    }
    status = file->GetInfo(file, &fi_guid, &fi_size, fi);
    if (EFI_ERROR(status))
    {
        ctx->bs->FreePool(fi);
        file->Close(file);
        root->Close(root);
        return status;
    }

    read_size = (UINTN)fi->FileSize;
    ctx->bs->FreePool(fi);

    status = ctx->bs->AllocatePool(EfiLoaderData, read_size, (void **)&buf);
    if (EFI_ERROR(status))
    {
        file->Close(file);
        root->Close(root);
        return status;
    }

    status = file->Read(file, &read_size, buf);
    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(status))
    {
        ctx->bs->FreePool(buf);
        return status;
    }

    *out = buf;
    *out_size = read_size;
    return EFI_SUCCESS;
}

EFI_STATUS
fs_load_file(struct boot_ctx *ctx, const CHAR16 *name, UINT8 **data,
             UINTN *size)
{
    EFI_STATUS status;
    EFI_GUID fs_guid = FileSystemProtocol;
    EFI_HANDLE *handles = NULL;
    UINTN n_handles = 0;

    status = ctx->bs->LocateHandleBuffer(ByProtocol, &fs_guid, NULL,
                                         &n_handles, &handles);
    if (EFI_ERROR(status) || n_handles == 0 || !handles)
        return EFI_NOT_FOUND;

    status = EFI_NOT_FOUND;
    for (UINTN i = 0; i < n_handles; i++)
    {
        if (!EFI_ERROR(try_volume(ctx, handles[i], name, data, size)))
        {
            status = EFI_SUCCESS;
            break;
        }
    }
    ctx->bs->FreePool(handles);
    return status;
}

EFI_STATUS
fs_load_kernel(struct boot_ctx *ctx)
{
    EFI_STATUS status;

    status = fs_load_file(ctx, L"\\kernel.elf",
                          &ctx->file_data, &ctx->file_size);
    if (EFI_ERROR(status))
    {
        bl_print("axboot: \\kernel.elf not found on any ESP\n");
        return status;
    }
    return EFI_SUCCESS;
}

/* Small INI-ish config reader for \axboot.cfg on the ESP.  Every non-empty,
   non-comment line is appended to the kernel command line verbatim (newlines
   become spaces); the loader also understands `splash=0/1` itself. */
void
axboot_config_load(struct boot_ctx *ctx)
{
    UINT8 *data = NULL;
    UINTN size = 0;
    UINTN n = 0;

    ctx->splash_enabled = 1;
    ctx->cmdline[0] = 0;

    if (EFI_ERROR(fs_load_file(ctx, L"\\axboot.cfg", &data, &size)) || !data)
    {
        bl_print("axboot: no \\axboot.cfg, using defaults\n");
        return;
    }

    for (UINTN i = 0; i < size; )
    {
        CHAR8 line[128];
        UINTN len = 0;

        while (i < size && data[i] != '\n' && len < sizeof(line) - 1)
            line[len++] = (CHAR8)data[i++];
        while (i < size && data[i] != '\n')
            i++;
        if (i < size)
            i++;
        line[len] = 0;

        /* Trim leading spaces / CR. */
        UINTN s = 0;
        while (s < len && (line[s] == ' ' || line[s] == '\t' ||
                           line[s] == '\r'))
            s++;
        UINTN e = len;
        while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t' ||
                         line[e - 1] == '\r'))
            e--;
        line[e] = 0;

        const CHAR8 *p = (const CHAR8 *)line + s;
        if (!*p || *p == '#')
            continue;

        UINTN plen = e - s;
        if (plen >= 6 && p[0] == 's' && p[1] == 'p' && p[2] == 'l' &&
            p[3] == 'a' && p[4] == 's' && p[5] == 'h')
        {
            const CHAR8 *v = p + 6;
            while (*v == ' ' || *v == '\t')
                v++;
            if (*v == '=')
            {
                v++;
                while (*v == ' ' || *v == '\t')
                    v++;
                if (*v == '0' || *v == 'n' || *v == 'N' || *v == 'f' ||
                    *v == 'F')
                    ctx->splash_enabled = 0;
            }
        }

        if (n && n + 1 < sizeof(ctx->cmdline))
            ctx->cmdline[n++] = ' ';
        for (const CHAR8 *q = p; *q && n + 1 < sizeof(ctx->cmdline); q++)
            ctx->cmdline[n++] = (char)*q;
    }
    ctx->cmdline[n] = 0;
    ctx->bs->FreePool(data);

    bl_print("axboot: config splash=");
    bl_print(ctx->splash_enabled ? "on" : "off");
    bl_print(" cmdline='");
    bl_print(ctx->cmdline);
    bl_print("'\n");
}
