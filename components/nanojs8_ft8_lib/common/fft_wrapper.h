#ifndef _INCLUDE_FFT_WRAPPER_H_
#define _INCLUDE_FFT_WRAPPER_H_

// NANOJS8 MODIFICATION (L7.5): originally `#include "../fft/kiss_fftr.h"`.
// We've removed Mini-FT8's bundled KissFFT (the entire `fft/` directory of
// the upstream Mini-FT8 component) to avoid duplicate-symbol link errors
// with the KissFFT already vendored in nanojs8_gfsk8 (used by gfsk8's
// JS8 modem). Both KissFFT copies are byte-identical; the gfsk8 vendored
// copy is exposed via its `vendor` include directory, hence the path
// `<kissfft/kiss_fftr.h>` below. ft8_lib's component CMakeLists declares
// `REQUIRES nanojs8_gfsk8` so this header is on the include path.
#include <kissfft/kiss_fftr.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int nfft;
    void* work;
    size_t work_size;
    kiss_fftr_cfg cfg;
} fft_plan_t;

int fft_plan_init_with_buffer(fft_plan_t* plan, int nfft, void* buffer, size_t buffer_size);
void fft_plan_free(fft_plan_t* plan);
void fft_execute(const fft_plan_t* plan, const kiss_fft_scalar* timedata, kiss_fft_cpx* freqdata);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_FFT_WRAPPER_H_
