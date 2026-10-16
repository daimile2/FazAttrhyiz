/* No libc or TLS; imported function/data and an R_X86_64_64 data pointer. */
extern long gold_value,*gold_relative,gold_answer(void);
long *gold_absolute=&gold_value;
int main(void)
{
    if(gold_answer()!=49 || gold_value!=42 || *gold_relative!=7 ||
       gold_absolute!=&gold_value || *gold_absolute!=42)return 71;
    static const char text[]="linux ldso: relocation verified\n";
    long r;__asm__ volatile("syscall":"=a"(r):"a"(1L),"D"(1L),"S"(text),"d"(sizeof(text)-1):"rcx","r11","memory");
    return r==sizeof(text)-1?0:72;
}
