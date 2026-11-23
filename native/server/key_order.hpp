// The order of the keys of the objects in a JSON text. The core's Json does not keep it, and named parameters
// are resolved in the order they were written (Python's dictionaries keep it), which decides which of two faults
// is reported; so the routes read it from the request's text and pass it with the request.
#ifndef KERFKITE_SERVER_KEY_ORDER_HPP
#define KERFKITE_SERVER_KEY_ORDER_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

struct KeyOrder {
    bool object = false, array = false;
    std::vector<std::string> keys;            // an object's keys, each where it first appears
    std::map<std::string, KeyOrder> members;  // the value of each key (its last appearance, as json.loads keeps)
    std::vector<KeyOrder> items;              // an array's items

    const KeyOrder *member(const std::string &name) const;
    const KeyOrder *item(size_t index) const;
    // The keys as a JSON array, or null when this is not an object.
    Json order() const;
};

// The structure of a JSON text; an empty KeyOrder when the text is not JSON.
KeyOrder key_order(std::string_view text);

// The order of an object reached by member names, or null.
Json order_at(const KeyOrder &root, std::initializer_list<const char *> path);

}  // namespace server
}  // namespace kerfkite

#endif
