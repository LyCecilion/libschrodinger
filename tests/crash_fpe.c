/* SIGFPE (FPE_INTDIV): integer division by zero -> "zero" wording. */
int main(void)
{
    volatile int a = 1;
    volatile int b = 0;
    return a / b;
}
