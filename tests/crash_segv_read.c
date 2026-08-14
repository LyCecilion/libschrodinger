/* SIGSEGV (SEGV_MAPERR): read from an unmapped address -> "read" wording. */
int main(void)
{
    volatile int *p = (volatile int *)0x1234;
    return *p;
}
