// The text members of a LightBurn user bundle, read in memory without Python: Studio's /api/import-bundle
// (studio/import_bundle.py's inspect_bundle), reproduced to the bit. See core/src/bundle_import.cpp for how the
// archive is read and what is declined (NotPorted).
#ifndef KERFKITE_BUNDLE_IMPORT_HPP
#define KERFKITE_BUNDLE_IMPORT_HPP

#include "kerfkite/json.hpp"

namespace kerfkite {

// "bundle-import": {"content": base64 text of the archive} -> {"files": [{"index", "name"}], "skipped"}, and
// with "index" -> {"name", "text"} of that member. Throws ValueError with the reference's message for an
// archive or member it refuses.
Json inspect_bundle(const Json &payload);

}  // namespace kerfkite

#endif
