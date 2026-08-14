/* SIGFPE (FPE_FLTUND): floating-point underflow -> "underflow" wording. */
#define _GNU_SOURCE
#include <fenv.h>
#include <float.h>

int main(void)
{
    feenableexcept(FE_UNDERFLOW);
    volatile double a = DBL_MIN;
    volatile double c = a / 2.0;
    (void)c;
    return 0;
}
