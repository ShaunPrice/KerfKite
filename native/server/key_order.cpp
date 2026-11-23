// The key order of a JSON text. See key_order.hpp.
#include "key_order.hpp"

#include <stdexcept>
#include <utility>

namespace kerfkite {
namespace server {

namespace {

struct Scanner {
    std::string_view text;
    size_t at = 0;
    int depth = 0;

    void space() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    }

    // A string's value, with its escapes decoded as far as keys need (\uXXXX pairs included).
    std::string string() {
        if (at >= text.size() || text[at] != '"') throw std::runtime_error("string");
        ++at;
        std::string out;
        while (at < text.size() && text[at] != '"') {
            const char c = text[at++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size()) throw std::runtime_error("escape");
            const char e = text[at++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    const auto hex4 = [&]() {
                        if (at + 4 > text.size()) throw std::runtime_error("escape");
                        unsigned v = std::stoul(std::string(text.substr(at, 4)), nullptr, 16);
                        at += 4;
                        return v;
                    };
                    unsigned code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff && at + 6 <= text.size() && text[at] == '\\' && text[at + 1] == 'u') {
                        at += 2;
                        const unsigned low = hex4();
                        if (low >= 0xdc00 && low <= 0xdfff) code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    }
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xc0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3f));
                    } else if (code < 0x10000) {
                        out += static_cast<char>(0xe0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                        out += static_cast<char>(0x80 | (code & 0x3f));
                    } else {
                        out += static_cast<char>(0xf0 | (code >> 18));
                        out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                        out += static_cast<char>(0x80 | (code & 0x3f));
                    }
                    break;
                }
                default: throw std::runtime_error("escape");
            }
        }
        if (at >= text.size()) throw std::runtime_error("string");
        ++at;
        return out;
    }

    KeyOrder value() {
        if (++depth > 512) throw std::runtime_error("depth");
        space();
        KeyOrder node;
        if (at >= text.size()) throw std::runtime_error("end");
        const char c = text[at];
        if (c == '{') {
            node.object = true;
            ++at;
            space();
            if (at < text.size() && text[at] == '}') {
                ++at;
            } else {
                for (;;) {
                    space();
                    const std::string key = string();
                    space();
                    if (at >= text.size() || text[at] != ':') throw std::runtime_error("colon");
                    ++at;
                    KeyOrder child = value();
                    if (!node.members.count(key)) node.keys.push_back(key);
                    node.members[key] = std::move(child);
                    space();
                    if (at < text.size() && text[at] == ',') {
                        ++at;
                        continue;
                    }
                    if (at < text.size() && text[at] == '}') {
                        ++at;
                        break;
                    }
                    throw std::runtime_error("object");
                }
            }
        } else if (c == '[') {
            node.array = true;
            ++at;
            space();
            if (at < text.size() && text[at] == ']') {
                ++at;
            } else {
                for (;;) {
                    node.items.push_back(value());
                    space();
                    if (at < text.size() && text[at] == ',') {
                        ++at;
                        continue;
                    }
                    if (at < text.size() && text[at] == ']') {
                        ++at;
                        break;
                    }
                    throw std::runtime_error("array");
                }
            }
        } else if (c == '"') {
            string();
        } else {
            // A number or a literal: skip to the next delimiter.
            while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']' && text[at] != ' ' && text[at] != '\n' &&
                   text[at] != '\t' && text[at] != '\r')
                ++at;
        }
        --depth;
        return node;
    }
};

}  // namespace

const KeyOrder *KeyOrder::member(const std::string &name) const {
    const auto found = members.find(name);
    return found == members.end() ? nullptr : &found->second;
}

const KeyOrder *KeyOrder::item(size_t index) const { return index < items.size() ? &items[index] : nullptr; }

Json KeyOrder::order() const {
    if (!object) return Json();
    Json out = Json::array();
    for (const std::string &key : keys) out.push_back(key);
    return out;
}

KeyOrder key_order(std::string_view text) {
    try {
        Scanner scanner{text};
        return scanner.value();
    } catch (const std::exception &) {
        return KeyOrder();
    }
}

Json order_at(const KeyOrder &root, std::initializer_list<const char *> path) {
    const KeyOrder *node = &root;
    for (const char *name : path) {
        node = node->member(name);
        if (!node) return Json();
    }
    return node->order();
}

}  // namespace server
}  // namespace kerfkite
