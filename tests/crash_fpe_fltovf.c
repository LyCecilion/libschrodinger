/* SIGFPE (FPE_FLTOVF): floating-point overflow -> "overflow" wording. */
#define _GNU_SOURCE
#include <fenv.h>
#include <float.h>

int main(void)
{
    feenableexcept(FE_OVERFLOW);
    volatile double a = DBL_MAX;
    volatile double c = a * a;
    (void)c;
    return 0;
}
