#include <stddef.h>

extern "C" {

int _fltused = 0;

void* memset(void* dst, int val, size_t count) {
    unsigned char* p = static_cast<unsigned char*>(dst);
    unsigned char v = static_cast<unsigned char>(val);
    for (size_t i = 0; i < count; ++i) {
        p[i] = v;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t count) {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t count) {
    const unsigned char* p1 = static_cast<const unsigned char*>(a);
    const unsigned char* p2 = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < count; ++i) {
        if (p1[i] != p2[i]) return (p1[i] < p2[i]) ? -1 : 1;
    }
    return 0;
}

const wchar_t* wcsstr(const wchar_t* hay, const wchar_t* needle) {
    if (!needle || !*needle) return hay;
    for (const wchar_t* h = hay; *h; ++h) {
        const wchar_t* p1 = h;
        const wchar_t* p2 = needle;
        while (*p1 && *p2 && *p1 == *p2) {
            ++p1;
            ++p2;
        }
        if (!*p2) return h;
    }
    return nullptr;
}

__attribute__((naked)) void __chkstk() {
    __asm__ volatile(
        "subq $16, %rsp\n"
        "movq %r10, (%rsp)\n"
        "movq %r11, 8(%rsp)\n"
        "leaq 24(%rsp), %r10\n"
        "cmpq $0x1000, %rax\n"
        "jb 2f\n"
        "1:\n"
        "subq $0x1000, %r10\n"
        "testl %eax, (%r10)\n"
        "subq $0x1000, %rax\n"
        "cmpq $0x1000, %rax\n"
        "ja 1b\n"
        "2:\n"
        "subq %rax, %r10\n"
        "testl %eax, (%r10)\n"
        "movq (%rsp), %r10\n"
        "movq 8(%rsp), %r11\n"
        "addq $16, %rsp\n"
        "ret\n"
    );
}

}
