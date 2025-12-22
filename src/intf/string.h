/**
 * @file string.h
 * @brief Basic memory and string manipulation functions for kernel
 *
 * Provides freestanding implementations of standard C library functions
 * such as memcpy, memset, memcmp, strlen, strcpy, strncpy, strcat, strcmp,
 * and strncmp.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

/// @name Memory Functions
/// @{

/**
 * @brief Copy memory from source to destination
 *
 * @param dest Destination pointer
 * @param src Source pointer
 * @param n Number of bytes to copy
 * @return Pointer to destination
 */
static inline void *memcpy(void *dest, const void *src, size_t n) {
    void *ret = dest;
    asm volatile(
        "cld\n\t"
        "rep movsb"
        : "+D"(dest), "+S"(src), "+c"(n)
        :
        : "memory");
    return ret;
}

/**
 * @brief Fill memory with a constant byte
 *
 * @param s Pointer to memory
 * @param c Byte value to set
 * @param n Number of bytes to set
 * @return Pointer to memory
 */
static inline void *memset(void *s, int c, size_t n) {
    void *ret = s;
    asm volatile(
        "cld\n\t"
        "rep stosb"
        : "+D"(s), "+c"(n)
        : "a"((uint8_t)c)
        : "memory");
    return ret;
}

/**
 * @brief Zero out memory
 *
 * @param ptr Pointer to memory
 * @param size Number of bytes to zero
 */
static inline void memzero(void *ptr, size_t size) {
    asm volatile(
        "cld\n\t"
        "xor %%al, %%al\n\t"
        "rep stosb"
        : "+D"(ptr), "+c"(size)
        :
        : "al", "memory");
}

/**
 * @brief Compare two memory regions
 *
 * @param s1 Pointer to first memory
 * @param s2 Pointer to second memory
 * @param n Number of bytes to compare
 * @return 0 if equal, negative if s1<s2, positive if s1>s2
 */
static inline int memcmp(const void *s1, const void *s2, size_t n) {
    if (n == 0) return 0;
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    unsigned char res = 0;

    asm volatile(
        "cld\n\t"
        "repe cmpsb\n\t"
        "setnz %0"
        : "=q"(res), "+S"(p1), "+D"(p2), "+c"(n)
        :
        : "cc");

    if (res == 0) return 0;
    return (int)(*(p1 - 1)) - (int)(*(p2 - 1));
}

/// @}

/// @name String Functions
/// @{

/**
 * @brief Compute length of a null-terminated string
 *
 * @param s Pointer to string
 * @return Number of characters before null terminator
 */
static inline size_t strlen(const char *s) {
    const char *p = s;
    size_t count = -1UL;
    asm volatile(
        "cld\n\t"
        "xor %%al, %%al\n\t"
        "repne scasb"
        : "+D"(p), "+c"(count)
        :
        : "memory");
    return -2UL - count;
}

/**
 * @brief Copy null-terminated string
 *
 * @param dest Destination buffer
 * @param src Source string
 * @return Pointer to destination
 */
static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

/**
 * @brief Copy string with maximum length
 *
 * Pads with null bytes if src is shorter than n
 *
 * @param dest Destination buffer
 * @param src Source string
 * @param n Maximum number of characters
 * @return Pointer to destination
 */
static inline char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n-- && (*d++ = *src++));
    while (n--) *d++ = '\0';
    return dest;
}

/**
 * @brief Concatenate null-terminated strings
 *
 * @param dest Destination buffer (must be large enough)
 * @param src Source string
 * @return Pointer to destination
 */
static inline char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

/**
 * @brief Compare two null-terminated strings
 *
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, negative if s1<s2, positive if s1>s2
 */
static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/**
 * @brief Compare up to n characters of two strings
 *
 * @param s1 First string
 * @param s2 Second string
 * @param n Maximum number of characters
 * @return 0 if equal, negative if s1<s2, positive if s1>s2
 */
static inline int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/**
 * @brief Locate first occurrence of character in string
 *
 * @param s Null-terminated string to search
 * @param c Character to find (interpreted as unsigned char)
 * @return Pointer to first occurrence of c, or nullptr if not found
 */
static inline char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    return (ch == 0) ? (char *)s : nullptr;
}

/// @}