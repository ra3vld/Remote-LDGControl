#pragma once
#include <stddef.h>
#include <string.h>

namespace ldg {
// Asset names are generated on the host; never expose arbitrary LittleFS paths.
inline bool validWebAsset(const char* name) {
    size_t prefix = 0;
    if (!strncmp(name, "widgets.", 8)) prefix = 8;
    else if (!strncmp(name, "settings.", 9)) prefix = 9;
    else if (!strncmp(name, "ru.", 3)) prefix = 3;
    if (!prefix || strlen(name) != prefix + 15 || strcmp(name + prefix + 12, ".js")) return false;
    for (size_t i = prefix; i < prefix + 12; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}
inline bool encodingToken(const char* begin, const char* end, const char* wanted) {
    if (size_t(end - begin) != strlen(wanted)) return false;
    while (begin != end) {
        const char value = *begin >= 'A' && *begin <= 'Z' ? *begin + ('a' - 'A') : *begin;
        if (value != *wanted++) return false;
        ++begin;
    }
    return true;
}
struct WebEncoding { bool gzip; bool identity; };
inline WebEncoding webEncoding(const char* header) {
    int gzip = -1, identity = -1, wildcard = -1;
    const char* p = header;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        const char* begin = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') ++p;
        const char* end = p;
        int quality = 1000;
        while (*p && *p != ',') {
            if ((*p == 'q' || *p == 'Q') && p[1] == '=') {
                p += 2;
                quality = 0;
                if (*p == '0' || *p == '1') {
                    quality = (*p++ - '0') * 1000;
                    if (*p == '.') {
                        ++p;
                        int place = 100;
                        while (*p >= '0' && *p <= '9') {
                            quality += (*p++ - '0') * place;
                            place /= 10;
                        }
                    }
                    if (quality > 1000 || (*p && *p != ',' && *p != ' ' && *p != '\t')) quality = 0;
                }
            } else ++p;
        }
        if (encodingToken(begin, end, "gzip")) gzip = quality;
        else if (encodingToken(begin, end, "identity")) identity = quality;
        else if (encodingToken(begin, end, "*")) wildcard = quality;
    }
    if (gzip < 0) gzip = wildcard > 0 ? wildcard : 0;
    if (identity < 0) identity = wildcard == 0 ? 0 : 1000;
    return {gzip > 0, identity > 0};
}
}
