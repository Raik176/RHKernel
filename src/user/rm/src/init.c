#include <stdint.h>
#include <unistd.h>
#include <string.h>
#define SYSCALL_UNLINK 17
static uint64_t sc3(uint64_t n,uint64_t a,uint64_t b,uint64_t c){uint64_t r; asm volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;}
static void es(const char*s){write(2,s,strlen(s));}
int main(int argc,char**argv){ if(argc<2){es("usage: rm <path>...\n"); return 1;} int rc=0; for(int i=1;i<argc;i++){ if((int64_t)sc3(SYSCALL_UNLINK,(uint64_t)argv[i],0,0)<0){es("rm: failed: "); es(argv[i]); es("\n"); rc=1;} } return rc; }
