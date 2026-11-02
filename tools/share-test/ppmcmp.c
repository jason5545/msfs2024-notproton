/* ppmcmp.c — native helper for mfgrab: finds column-periodic channel errors in a decoded frame.
 *
 *   ppmcmp <test.ppm> [<reference.ppm>] [period]
 *
 * With a reference: per channel, mean |test - ref| over the whole frame, then for each column residue x mod period
 * (default 16) the mean signed and absolute difference. Without one: per residue, the mean of |c(x) - (c(x-1) + c(x+1)) / 2|
 * (horizontal roughness) per channel, which shows stripes without knowing the right picture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb"); char magic[3] = {0}; int maxv; unsigned char *p;
    if (!f || fscanf(f, "%2s %d %d %d", magic, w, h, &maxv) != 4 || strcmp(magic, "P6") || maxv != 255)
    { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    fgetc(f);
    p = malloc((size_t)*w * *h * 3);
    if (fread(p, 1, (size_t)*w * *h * 3, f) != (size_t)*w * *h * 3) { fprintf(stderr, "short %s\n", path); exit(1); }
    fclose(f);
    return p;
}

int main(int argc, char **argv)
{
    int w, h, rw, rh, period = 16; unsigned char *t, *r = NULL; const char *names = "RGB";
    if (argc < 2) { fprintf(stderr, "usage: ppmcmp test.ppm [ref.ppm] [period]\n"); return 1; }
    t = load(argv[1], &w, &h);
    if (argc > 2 && strchr(argv[2], '.')) { r = load(argv[2], &rw, &rh); if (rw != w || rh != h) { fprintf(stderr, "size mismatch\n"); return 1; } }
    if (argc > (r ? 3 : 2)) period = atoi(argv[r ? 3 : 2]);
    double *sum = calloc(period * 3, sizeof(double)), *asum = calloc(period * 3, sizeof(double)), tot[3] = {0};
    long *cnt = calloc(period, sizeof(long)); long all = 0;
    for (int y = 0; y < h; y++)
        for (int x = 1; x < w - 1; x++)
        {
            size_t i = ((size_t)y * w + x) * 3; int m = x % period;
            for (int c = 0; c < 3; c++)
            {
                double d;
                if (r) d = (double)t[i + c] - r[i + c];
                else d = t[i + c] - (t[i - 3 + c] + t[i + 3 + c]) / 2.0;
                sum[m * 3 + c] += d; asum[m * 3 + c] += d < 0 ? -d : d; tot[c] += d < 0 ? -d : d;
            }
            cnt[m]++; all++;
        }
    printf("%s %s %dx%d, period %d\n", argv[1], r ? argv[2] : "(no reference: horizontal roughness)", w, h, period);
    printf("whole frame mean |%s|: R %.2f G %.2f B %.2f\n", r ? "test-ref" : "rough", tot[0] / all, tot[1] / all, tot[2] / all);
    printf("x mod %-3d %s\n", period, r ? "  signed R  G  B        |abs| R  G  B" : "  rough R  G  B");
    for (int m = 0; m < period; m++)
    {
        if (r) printf("  %3d   %7.2f %7.2f %7.2f   %7.2f %7.2f %7.2f\n", m, sum[m * 3] / cnt[m], sum[m * 3 + 1] / cnt[m], sum[m * 3 + 2] / cnt[m],
                      asum[m * 3] / cnt[m], asum[m * 3 + 1] / cnt[m], asum[m * 3 + 2] / cnt[m]);
        else printf("  %3d   %7.2f %7.2f %7.2f\n", m, asum[m * 3] / cnt[m], asum[m * 3 + 1] / cnt[m], asum[m * 3 + 2] / cnt[m]);
    }
    for (int c = 0; c < 3; c++) (void)names[c];
    return 0;
}
