// The error an invalid input is refused with, apart from cam.hpp so that a part of the core which has
// nothing to do with JSON (the solid kernel) can throw it without including the JSON library.
#ifndef KERFKITE_ERRORS_HPP
#define KERFKITE_ERRORS_HPP

#include <stdexcept>

namespace kerfkite {

// An input the reference rejects. what() is the reference's message.
struct ValueError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace kerfkite

#endif
