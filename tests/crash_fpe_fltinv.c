/* SIGFPE (FPE_FLTINV): invalid floating-point operation -> "invalid" wording. */
#define _GNU_SOURCE
#include <fenv.h>

int main(void)
{
    feenableexcept(FE_INVALID);
    volatile double a = 0.0;
    volatile double c = a / a;
    (void)c;
    return 0;
}
