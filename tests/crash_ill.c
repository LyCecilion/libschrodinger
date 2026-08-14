/* SIGILL (ILL_ILLOPC): execute the ud2 illegal instruction -> "execute". */
int main(void)
{
    __asm__ volatile("ud2");
    return 0;
}
