#include <stdint.h>
#include <unistd.h>
#include <string.h>
#define SYSCALL_RENAME 18
static uint64_t sc3(uint64_t n,uint64_t a,uint64_t b,uint64_t c){uint64_t r; asm volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;}
static void es(const char*s){write(2,s,strlen(s));}
int main(int argc,char**argv){ if(argc!=3){es("usage: mv <old> <new>\n"); return 1;} if((int64_t)sc3(SYSCALL_RENAME,(uint64_t)argv[1],(uint64_t)argv[2],0)<0){es("mv: rename failed\n"); return 1;} return 0; }
