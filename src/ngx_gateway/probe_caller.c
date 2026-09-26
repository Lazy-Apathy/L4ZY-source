/* MinGW probe: LoadLibrary sauer_ngx.dll and call the versioned C exports.
 * Does not link NVIDIA static libraries. Does not use guessed nvngx.dll signatures.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include "ngx_gateway/sauer_ngx.h"

static void *must(HMODULE m, const char *name)
{
    void *p = (void *)GetProcAddress(m, name);
    if(!p) printf("MISSING EXPORT %s (GetLastError=%lu)\n", name, (unsigned long)GetLastError());
    return p;
}

int main(int argc, char **argv)
{
    const char *dll = (argc > 1) ? argv[1] : "sauer_ngx.dll";
    const char *outpath = (argc > 2) ? argv[2] : "probe-link.txt";
    printf("LoadLibrary %s\n", dll);
    HMODULE m = LoadLibraryA(dll);
    if(!m)
    {
        printf("LoadLibrary failed %lu\n", (unsigned long)GetLastError());
        return 2;
    }

    PFN_sauer_ngx_abi_version p_ver = (PFN_sauer_ngx_abi_version)must(m, "sauer_ngx_abi_version");
    PFN_sauer_ngx_probe_link p_probe = (PFN_sauer_ngx_probe_link)must(m, "sauer_ngx_probe_link");
    PFN_sauer_ngx_last_error p_err = (PFN_sauer_ngx_last_error)must(m, "sauer_ngx_last_error");
    PFN_sauer_ngx_set_paths p_paths = (PFN_sauer_ngx_set_paths)must(m, "sauer_ngx_set_paths");
    PFN_sauer_ngx_query_instance_exts p_iext = (PFN_sauer_ngx_query_instance_exts)must(m, "sauer_ngx_query_instance_exts");
    PFN_sauer_ngx_query_optimal p_opt = (PFN_sauer_ngx_query_optimal)must(m, "sauer_ngx_query_optimal");
    PFN_sauer_ngx_create_feature p_cf = (PFN_sauer_ngx_create_feature)must(m, "sauer_ngx_create_feature");
    (void)p_opt;
    (void)p_cf;
    if(!p_ver || !p_probe || !p_err || !p_opt || !p_cf)
    {
        FreeLibrary(m);
        return 3;
    }

    uint32_t ver = p_ver();
    char probe[1024];
    memset(probe, 0, sizeof(probe));
    int32_t pr = p_probe(probe, (uint32_t)sizeof(probe));
    printf("abi_version=%u (header %u)\n", ver, SAUER_NGX_ABI_VERSION);
    printf("probe_link rc=%d\n%s\n", (int)pr, probe);

    SauerNgxError err;
    memset(&err, 0, sizeof(err));
    err.struct_bytes = sizeof(err);
    p_err(&err, sizeof(err));
    printf("last_error code=%d ngx=0x%08X %s\n", (int)err.code, err.ngx_result, err.message);

    int rc = 0;
    if(ver != SAUER_NGX_ABI_VERSION) rc = 4;
    if(pr != SAUER_NGX_OK) rc = 5;

    if(p_paths && argc > 3)
    {
        SauerNgxPaths paths;
        memset(&paths, 0, sizeof(paths));
        paths.struct_bytes = sizeof(paths);
        strncpy(paths.dll_dir, argv[3], SAUER_NGX_PATH - 1);
        if(argc > 4) strncpy(paths.log_dir, argv[4], SAUER_NGX_PATH - 1);
        int32_t sr = p_paths(&paths, sizeof(paths));
        printf("set_paths rc=%d dir=%s\n", (int)sr, paths.dll_dir);
        if(p_iext && sr == SAUER_NGX_OK)
        {
            SauerNgxExtList exts;
            memset(&exts, 0, sizeof(exts));
            exts.struct_bytes = sizeof(exts);
            int32_t ir = p_iext(&exts, sizeof(exts));
            printf("query_instance_exts rc=%d count=%u\n", (int)ir, exts.count);
            for(uint32_t i = 0; i < exts.count; i++) printf("  %s\n", exts.names[i]);
            memset(&err, 0, sizeof(err));
            err.struct_bytes = sizeof(err);
            p_err(&err, sizeof(err));
            printf("after query: code=%d ngx=0x%08X %s\n", (int)err.code, err.ngx_result, err.message);
        }
    }

    FILE *f = fopen(outpath, "w");
    if(f)
    {
        fprintf(f, "dll %s\n", dll);
        fprintf(f, "LoadLibrary ok\n");
        fprintf(f, "abi_version %u header %u\n", ver, SAUER_NGX_ABI_VERSION);
        fprintf(f, "probe_link rc %d\n%s\n", (int)pr, probe);
        fprintf(f, "last_error code %d ngx 0x%08X %s\n", (int)err.code, err.ngx_result, err.message);
        fprintf(f, "streamline_loaded %s\n", GetModuleHandleA("sl.interposer.dll") ? "yes" : "no");
        fclose(f);
        printf("wrote %s\n", outpath);
    }
    FreeLibrary(m);
    return rc;
}
