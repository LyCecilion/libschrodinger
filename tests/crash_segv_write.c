/* SIGSEGV (SEGV_ACCERR): write to a read-only page (.rodata) -> "written". */
int main(void)
{
    char *s = "read-only";
    s[0] = 'X';
    return 0;
}
