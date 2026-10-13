// Demo: libc++ small-string layout as observed in 0x102d49d80
// (len < 0x17 stored inline with tagged length; else heap + memmove).
// Build: make sso_string_demo
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static void assign_like_105d49d80(std::string& out, const char* data, size_t len) {
    out.assign(data, len); // libc++ SSO: inline iff len < 23
}

int main() {
    std::string a, b;
    assign_like_105d49d80(a, "Focused", 7);          // inline
    const char big[] = "Mac-RobloxPlayer-SessionReport-Inferred-0123456789";
    assign_like_105d49d80(b, big, sizeof(big) - 1);  // heap
    std::printf("a='%s' (len=%zu) b(len=%zu)\n", a.c_str(), a.size(), b.size());
    assert(a == "Focused" && b == big);
    std::puts("sso ok");
    return 0;
}
