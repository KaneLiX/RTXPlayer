// Development tool (not shipped): luma PSNR and SSIM between two raw BGRA images.
//   imgcmp <ref.bgra> <test.bgra> <w> <h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static std::vector<float> Luma(const char* path, int w, int h) {
    std::vector<unsigned char> b((size_t)w * h * 4);
    FILE* f = fopen(path, "rb");
    if (!f || fread(b.data(), 1, b.size(), f) != b.size()) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(3);
    }
    fclose(f);
    std::vector<float> y((size_t)w * h);
    for (size_t i = 0; i < y.size(); ++i)
        y[i] = 0.0722f * b[4 * i] + 0.7152f * b[4 * i + 1] + 0.2126f * b[4 * i + 2];
    return y;
}

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    int w = atoi(argv[3]), h = atoi(argv[4]);
    auto a = Luma(argv[1], w, h), b = Luma(argv[2], w, h);
    double se = 0;
    for (size_t i = 0; i < a.size(); ++i) se += (a[i] - b[i]) * (a[i] - b[i]);
    double psnr = 10 * std::log10(255.0 * 255.0 / (se / a.size()));
    // SSIM over 8x8 blocks (stride 4).
    const double c1 = 6.5025, c2 = 58.5225;
    double ssim = 0;
    int n = 0;
    for (int y = 0; y + 8 <= h; y += 4)
        for (int x = 0; x + 8 <= w; x += 4) {
            double ma = 0, mb = 0, va = 0, vb = 0, cov = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    size_t k = (size_t)(y + j) * w + x + i;
                    ma += a[k];
                    mb += b[k];
                }
            ma /= 64;
            mb /= 64;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    size_t k = (size_t)(y + j) * w + x + i;
                    va += (a[k] - ma) * (a[k] - ma);
                    vb += (b[k] - mb) * (b[k] - mb);
                    cov += (a[k] - ma) * (b[k] - mb);
                }
            va /= 63;
            vb /= 63;
            cov /= 63;
            ssim += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    printf("PSNR %.2f dB  SSIM %.4f\n", psnr, ssim / n);
    return 0;
}
