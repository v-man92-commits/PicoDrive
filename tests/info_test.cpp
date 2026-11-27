#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "common.h"
#include "ImageInfo.h"
int main(int argc, char** argv) {
    int fails = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        struct stat st; stat(argv[i], &st);
        ImageFormat f; imageInfo_detect(argv[i], (u32)st.st_size, f);
        bool ok = strstr(f.name, argv[i+1]) != nullptr;
        printf("%-4s %-18s -> %-26s | %-18s | %-12s %s\n", ok ? "ok" : "FAIL", argv[i] + 5, f.name, f.detail, f.label, f.isIso ? "(disc)" : "");
        fails += !ok;
    }
    printf("%d failures\n", fails);
    return fails != 0;
}
