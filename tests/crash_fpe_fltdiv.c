/* SIGFPE (FPE_FLTDIV): floating-point division by zero -> "divide" wording. */
#define _GNU_SOURCE
#include <fenv.h>

int main(void)
{
    feenableexcept(FE_DIVBYZERO);
    volatile double a = 1.0;
    volatile double b = 0.0;
    volatile double c = a / b;
    (void)c;
    return 0;
}
