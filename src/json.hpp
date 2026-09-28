#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdlib>

// Small strict JSON reader for ffprobe's machine-readable output.
namespace json {
struct Value {
    std::string text;
    std::vector<Value> array;
    std::map<std::string, Value> object;
    const Value &operator[](const std::string &key) const {
        static const Value empty;
        auto it = object.find(key);
        return it == object.end() ? empty : it->second;
    }
    double number(double fallback = 0) const {
        if (text.empty())
            return fallback;
        char *end = nullptr;
        double n = std::strtod(text.c_str(), &end);
        return end == text.c_str() + text.size() ? n : fallback;
    }
};
class Reader {
    std::string_view s;
    size_t p = 0;
    unsigned depth = 0;
    void space() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t'))
            ++p;
    }
    char get() {
        if (p >= s.size())
            throw std::runtime_error("Invalid metadata");
        return s[p++];
    }
    void expect(char c) {
        space();
        if (get() != c)
            throw std::runtime_error("Invalid metadata");
    }
    unsigned hex4() {
        unsigned n = 0;
        for (int i = 0; i < 4; i++) {
            char c = get();
            n <<= 4;
            if (c >= '0' && c <= '9')
                n += c - '0';
            else if (c >= 'a' && c <= 'f')
                n += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                n += c - 'A' + 10;
            else
                throw std::runtime_error("Invalid metadata");
        }
        return n;
    }
    static void utf8(std::string &out, unsigned n) {
        if (n < 128)
            out.push_back(char(n));
        else if (n < 2048) {
            out.push_back(char(0xc0 | (n >> 6)));
            out.push_back(char(0x80 | (n & 63)));
        } else if (n < 65536) {
            out.push_back(char(0xe0 | (n >> 12)));
            out.push_back(char(0x80 | ((n >> 6) & 63)));
            out.push_back(char(0x80 | (n & 63)));
        } else {
            out.push_back(char(0xf0 | (n >> 18)));
            out.push_back(char(0x80 | ((n >> 12) & 63)));
            out.push_back(char(0x80 | ((n >> 6) & 63)));
            out.push_back(char(0x80 | (n & 63)));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            char c = get();
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 32)
                throw std::runtime_error("Invalid metadata");
            if (c != '\\') {
                out += c;
                continue;
            }
            switch (get()) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                unsigned n = hex4();
                if (n >= 0xd800 && n <= 0xdbff) {
                    if (get() != '\\' || get() != 'u')
                        throw std::runtime_error("Invalid metadata");
                    unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        throw std::runtime_error("Invalid metadata");
                    n = 0x10000 + ((n - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, n);
                break;
            }
            default:
                throw std::runtime_error("Invalid metadata");
            }
        }
    }
    Value value() {
        space();
        if (++depth > 48 || p >= s.size())
            throw std::runtime_error("Invalid metadata");
        Value v;
        char c = s[p];
        if (c == '{') {
            ++p;
            space();
            if (p < s.size() && s[p] == '}')
                ++p;
            else
                for (;;) {
                    auto key = string();
                    expect(':');
                    v.object.emplace(std::move(key), value());
                    space();
                    char end = get();
                    if (end == '}')
                        break;
                    if (end != ',')
                        throw std::runtime_error("Invalid metadata");
                }
        } else if (c == '[') {
            ++p;
            space();
            if (p < s.size() && s[p] == ']')
                ++p;
            else
                for (;;) {
                    v.array.push_back(value());
                    space();
                    char end = get();
                    if (end == ']')
                        break;
                    if (end != ',')
                        throw std::runtime_error("Invalid metadata");
                }
        } else if (c == '"')
            v.text = string();
        else {
            size_t start = p;
            while (p < s.size() && s[p] != ',' && s[p] != ']' && s[p] != '}' && s[p] != ' ' && s[p] != '\r' &&
                   s[p] != '\n' && s[p] != '\t')
                ++p;
            v.text = std::string(s.substr(start, p - start));
            if (v.text.empty())
                throw std::runtime_error("Invalid metadata");
        }
        --depth;
        return v;
    }

  public:
    explicit Reader(std::string_view input) : s(input) {}
    Value read() {
        auto v = value();
        space();
        if (p != s.size())
            throw std::runtime_error("Invalid metadata");
        return v;
    }
};
inline Value parse(std::string_view s) {
    return Reader(s).read();
}
} // namespace json
