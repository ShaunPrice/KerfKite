// Files of the editor, served as Flask's send_from_directory serves them: the path joined safely to the
// directory (no ".." or absolute parts), its type from the extension, 404 when it is not a file.
#ifndef KERFKITE_SERVER_STATIC_FILES_HPP
#define KERFKITE_SERVER_STATIC_FILES_HPP

#include <string>

#include "http.hpp"

namespace kerfkite {
namespace server {

http::Response static_file(const std::string &directory, const std::string &relative);

// The editor's directory: given, else beside the program (share/kerfkite/static, static) or the source tree's.
std::string find_static_dir(const std::string &given, const char *argv0);

}  // namespace server
}  // namespace kerfkite

#endif
