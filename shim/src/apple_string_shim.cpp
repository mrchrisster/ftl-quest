#include "apple_string_shim.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>

// Exact layout of Apple ARM64 libc++ std::__1::basic_string<char>
// (_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT, little-endian aarch64)
struct AppleString {
    struct LongRep {
        char *__data_;
        size_t __size_;
        size_t __cap_ : 63;
        size_t __is_long_ : 1;
    };

    static constexpr size_t kMinCap = 23; // (sizeof(LongRep) - 1) / sizeof(char)

    struct ShortRep {
        char __data_[kMinCap];
        uint8_t __size_ : 7;
        uint8_t __is_long_ : 1;
    };

    union Rep {
        LongRep l;
        ShortRep s;
    } rep;

    AppleString() noexcept {
        rep.s.__data_[0] = '\0';
        rep.s.__size_ = 0;
        rep.s.__is_long_ = 0;
    }

    AppleString(const AppleString &other);
    ~AppleString();

    bool is_long() const noexcept { return rep.s.__is_long_ != 0; }

    char *data_mut() noexcept { return is_long() ? rep.l.__data_ : rep.s.__data_; }
    const char *data() const noexcept { return is_long() ? rep.l.__data_ : rep.s.__data_; }

    size_t size() const noexcept { return is_long() ? rep.l.__size_ : rep.s.__size_; }
    size_t capacity() const noexcept { return is_long() ? (rep.l.__cap_ - 1) : (kMinCap - 1); }

    void set_size(size_t n) noexcept {
        if (is_long()) {
            rep.l.__size_ = n;
        } else {
            rep.s.__size_ = static_cast<uint8_t>(n);
            rep.s.__is_long_ = 0;
        }
    }

    static size_t recommend_alloc(size_t need_chars) noexcept {
        // Allocate at least need_chars + 1, rounded up to 16 bytes
        size_t bytes = (need_chars + 1 + 15) & ~size_t(15);
        return bytes < 32 ? 32 : bytes;
    }

    void init_from(const char *s, size_t n) {
        if (n < kMinCap) {
            if (n > 0) memcpy(rep.s.__data_, s, n);
            rep.s.__data_[n] = '\0';
            rep.s.__size_ = static_cast<uint8_t>(n);
            rep.s.__is_long_ = 0;
        } else {
            size_t alloc_cap = recommend_alloc(n);
            char *buf = static_cast<char *>(::operator new(alloc_cap));
            memcpy(buf, s, n);
            buf[n] = '\0';
            rep.l.__data_ = buf;
            rep.l.__size_ = n;
            rep.l.__cap_ = alloc_cap;
            rep.l.__is_long_ = 1;
        }
    }

    void destroy_buffer() noexcept {
        if (is_long() && rep.l.__data_) {
            ::operator delete(rep.l.__data_);
        }
    }
};

static_assert(sizeof(AppleString) == 24, "AppleString must be 24 bytes");

AppleString::AppleString(const AppleString &other) {
    init_from(other.data(), other.size());
}

AppleString::~AppleString() {
    destroy_buffer();
}

static int mem_compare_range(const char *p1, size_t n1, const char *p2, size_t n2) {
    size_t rlen = std::min(n1, n2);
    int cmp = (rlen > 0) ? memcmp(p1, p2, rlen) : 0;
    if (cmp != 0) return cmp;
    if (n1 < n2) return -1;
    if (n1 > n2) return 1;
    return 0;
}

static bool ptr_aliases_buffer(const char *s, const char *buf, size_t len) {
    uintptr_t p = reinterpret_cast<uintptr_t>(s);
    uintptr_t lo = reinterpret_cast<uintptr_t>(buf);
    uintptr_t hi = lo + len + 1;
    return p >= lo && p < hi;
}

// ============================================================================
// Shimmed Apple libc++ std::__1::basic_string<char> methods
// ============================================================================

static AppleString *apple_str_ctor_copy(AppleString *self, const AppleString *other) {
    self->init_from(other->data(), other->size());
    return self;
}

static AppleString *apple_str_ctor_substr(
    AppleString *self, const AppleString *other, size_t pos, size_t n, const void * /*alloc*/
) {
    size_t olen = other->size();
    if (pos > olen) pos = olen;
    size_t rlen = std::min(n, olen - pos);
    self->init_from(other->data() + pos, rlen);
    return self;
}

static AppleString *apple_str_dtor(AppleString *self) {
    self->destroy_buffer();
    return self;
}

static size_t apple_str_find_char(const AppleString *self, char c, size_t pos) {
    size_t len = self->size();
    if (pos >= len) return static_cast<size_t>(-1);
    const char *buf = self->data();
    const void *found = memchr(buf + pos, static_cast<unsigned char>(c), len - pos);
    if (!found) return static_cast<size_t>(-1);
    return static_cast<size_t>(static_cast<const char *>(found) - buf);
}

static int apple_str_compare_cstr(const AppleString *self, const char *s) {
    size_t n2 = s ? strlen(s) : 0;
    return mem_compare_range(self->data(), self->size(), s, n2);
}

static int apple_str_compare_range(
    const AppleString *self, size_t pos1, size_t n1, const char *s, size_t n2
) {
    size_t len1 = self->size();
    if (pos1 > len1) pos1 = len1;
    size_t rlen1 = std::min(n1, len1 - pos1);
    return mem_compare_range(self->data() + pos1, rlen1, s, n2);
}

static AppleString *apple_str_replace_range(
    AppleString *self, size_t pos, size_t n1, const char *s, size_t n2
) {
    size_t old_len = self->size();
    if (pos > old_len) pos = old_len;
    size_t rlen = std::min(n1, old_len - pos);
    size_t new_len = old_len - rlen + n2;
    size_t suffix_len = old_len - (pos + rlen);

    char *old_buf = self->data_mut();
    size_t cap = self->capacity();

    if (new_len <= cap) {
        std::string tmp;
        const char *src = s;
        if (n2 > 0 && ptr_aliases_buffer(s, old_buf, old_len)) {
            tmp.assign(s, n2);
            src = tmp.data();
        }
        if (suffix_len > 0 && n2 != rlen) {
            memmove(old_buf + pos + n2, old_buf + pos + rlen, suffix_len);
        }
        if (n2 > 0) {
            memcpy(old_buf + pos, src, n2);
        }
        old_buf[new_len] = '\0';
        self->set_size(new_len);
        return self;
    }

    size_t new_cap = std::max(AppleString::recommend_alloc(new_len),
                              AppleString::recommend_alloc(cap * 2));
    char *new_buf = static_cast<char *>(::operator new(new_cap));
    if (pos > 0) memcpy(new_buf, old_buf, pos);
    if (n2 > 0) memcpy(new_buf + pos, s, n2);
    if (suffix_len > 0) memcpy(new_buf + pos + n2, old_buf + pos + rlen, suffix_len);
    new_buf[new_len] = '\0';

    self->destroy_buffer();
    self->rep.l.__data_ = new_buf;
    self->rep.l.__size_ = new_len;
    self->rep.l.__cap_ = new_cap;
    self->rep.l.__is_long_ = 1;
    return self;
}

static AppleString *apple_str_assign_str(AppleString *self, const AppleString *other) {
    if (self == other) return self;
    size_t n = other->size();
    const char *src = other->data();
    return apple_str_replace_range(self, 0, self->size(), src, n);
}

static AppleString *apple_str_assign_cstr(AppleString *self, const char *s) {
    size_t n = s ? strlen(s) : 0;
    return apple_str_replace_range(self, 0, self->size(), s, n);
}

static AppleString *apple_str_append_cstr_n(AppleString *self, const char *s, size_t n) {
    return apple_str_replace_range(self, self->size(), 0, s, n);
}

static AppleString *apple_str_append_cstr(AppleString *self, const char *s) {
    size_t n = s ? strlen(s) : 0;
    return apple_str_replace_range(self, self->size(), 0, s, n);
}

static AppleString *apple_str_insert_cstr_n(AppleString *self, size_t pos, const char *s, size_t n) {
    return apple_str_replace_range(self, pos, 0, s, n);
}

static AppleString *apple_str_insert_cstr(AppleString *self, size_t pos, const char *s) {
    size_t n = s ? strlen(s) : 0;
    return apple_str_replace_range(self, pos, 0, s, n);
}

static AppleString *apple_str_replace_cstr(AppleString *self, size_t pos, size_t n1, const char *s) {
    size_t n2 = s ? strlen(s) : 0;
    return apple_str_replace_range(self, pos, n1, s, n2);
}

static AppleString *apple_str_erase(AppleString *self, size_t pos, size_t n) {
    return apple_str_replace_range(self, pos, n, nullptr, 0);
}

static void apple_str_reserve(AppleString *self, size_t res_arg) {
    size_t cap = self->capacity();
    if (res_arg <= cap) return;
    size_t len = self->size();
    const char *old_buf = self->data();
    size_t new_cap = AppleString::recommend_alloc(res_arg);
    char *new_buf = static_cast<char *>(::operator new(new_cap));
    if (len > 0) memcpy(new_buf, old_buf, len);
    new_buf[len] = '\0';

    self->destroy_buffer();
    self->rep.l.__data_ = new_buf;
    self->rep.l.__size_ = len;
    self->rep.l.__cap_ = new_cap;
    self->rep.l.__is_long_ = 1;
}

static void apple_str_resize(AppleString *self, size_t n, char c) {
    size_t len = self->size();
    if (n <= len) {
        char *buf = self->data_mut();
        buf[n] = '\0';
        self->set_size(n);
        return;
    }
    if (n > self->capacity()) {
        apple_str_reserve(self, n);
    }
    char *buf = self->data_mut();
    memset(buf + len, static_cast<unsigned char>(c), n - len);
    buf[n] = '\0';
    self->set_size(n);
}

static void apple_str_push_back(AppleString *self, char c) {
    size_t len = self->size();
    if (len >= self->capacity()) {
        apple_str_reserve(self, std::max(len + 1, self->capacity() * 2));
    }
    char *buf = self->data_mut();
    buf[len] = c;
    buf[len + 1] = '\0';
    self->set_size(len + 1);
}

// Returned indirectly via x8 per AAPCS64 (non-trivial destructor/copy ctor)
static AppleString apple_str_concat_cstr_str(const char *lhs, const AppleString &rhs) {
    AppleString res;
    size_t l_len = lhs ? strlen(lhs) : 0;
    size_t r_len = rhs.size();
    size_t total = l_len + r_len;
    if (total < AppleString::kMinCap) {
        if (l_len > 0) memcpy(res.rep.s.__data_, lhs, l_len);
        if (r_len > 0) memcpy(res.rep.s.__data_ + l_len, rhs.data(), r_len);
        res.rep.s.__data_[total] = '\0';
        res.rep.s.__size_ = static_cast<uint8_t>(total);
        res.rep.s.__is_long_ = 0;
    } else {
        size_t alloc_cap = AppleString::recommend_alloc(total);
        char *buf = static_cast<char *>(::operator new(alloc_cap));
        if (l_len > 0) memcpy(buf, lhs, l_len);
        if (r_len > 0) memcpy(buf + l_len, rhs.data(), r_len);
        buf[total] = '\0';
        res.rep.l.__data_ = buf;
        res.rep.l.__size_ = total;
        res.rep.l.__cap_ = alloc_cap;
        res.rep.l.__is_long_ = 1;
    }
    return res;
}

void register_apple_string_symbols(std::unordered_map<std::string, void *> &m) {
    m["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm"] = (void *)apple_str_find_char;
    m["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc"] = (void *)apple_str_compare_cstr;
    m["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm"] = (void *)apple_str_compare_range;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm"] = (void *)apple_str_erase;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc"] = (void *)apple_str_append_cstr;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm"] = (void *)apple_str_append_cstr_n;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc"] = (void *)apple_str_assign_cstr;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc"] = (void *)apple_str_insert_cstr;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKcm"] = (void *)apple_str_insert_cstr_n;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc"] = (void *)apple_str_resize;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7replaceEmmPKc"] = (void *)apple_str_replace_cstr;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7replaceEmmPKcm"] = (void *)apple_str_replace_range;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm"] = (void *)apple_str_reserve;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc"] = (void *)apple_str_push_back;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_"] = (void *)apple_str_ctor_copy;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_"] = (void *)apple_str_ctor_substr;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev"] = (void *)apple_str_dtor;
    m["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_"] = (void *)apple_str_assign_str;
    m["__ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_"] = (void *)apple_str_concat_cstr_str;
}
