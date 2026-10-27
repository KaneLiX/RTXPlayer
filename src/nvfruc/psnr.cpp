// Development tool (not shipped): luma PSNR between two raw I420 files, averaged
// separately over even (source) and odd (interpolated) frame indices.
//   psnr <a.yuv> <b.yuv> <w> <h> [prev]   prev: compare b[i] with a[i-1] (= frame repetition baseline)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    int w = atoi(argv[3]), h = atoi(argv[4]);
    bool prev = argc > 5 && strcmp(argv[5], "prev") == 0;
    int offset = argc > 6 ? atoi(argv[6]) : 0;  // skip this many frames of b first (alignment search)
    bool sub2 = argc > 5 && strcmp(argv[5], "sub2") == 0;  // compare a[2i] with b[i]
    size_t fs = (size_t)w * h * 3 / 2;
    FILE* fa = fopen(argv[1], "rb");
    FILE* fb = fopen(argv[2], "rb");
    if (!fa || !fb) return 3;
    std::vector<unsigned char> a(fs), b(fs), lastA(fs);
    double sum[2] = {}, worst[2] = {99, 99};
    int cnt[2] = {};
    for (int s = 0; s < offset; ++s) fread(b.data(), 1, fs, fb);
    for (int i = 0;; ++i) {
        lastA = a;
        if (sub2 && i > 0 && fread(a.data(), 1, fs, fa) != fs) break;
        if (fread(a.data(), 1, fs, fa) != fs || fread(b.data(), 1, fs, fb) != fs) break;
        if (prev && i == 0) continue;
        const unsigned char* ref = prev ? lastA.data() : a.data();
        double se = 0;
        for (size_t k = 0; k < (size_t)w * h; ++k) {
            int d = ref[k] - b[k];
            se += d * d;
        }
        double mse = se / ((double)w * h);
        double p = mse == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
        sum[i & 1] += p;
        ++cnt[i & 1];
        if (p < worst[i & 1]) worst[i & 1] = p;
    }
    printf("even(source) %.2f dB  odd(interp) %.2f dB  worst odd %.2f dB  (%d+%d frames)\n",
           cnt[0] ? sum[0] / cnt[0] : 0, cnt[1] ? sum[1] / cnt[1] : 0, worst[1], cnt[0], cnt[1]);
    return 0;
}
