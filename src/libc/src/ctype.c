#include <ctype.h>

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isspace(int c) { return isblank(c) || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int isgraph(int c) { return c > 0x20 && c < 0x7f; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
