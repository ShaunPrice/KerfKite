// The vector paths of one PDF page as an import review draft, without Python: Studio's /api/pdf-import
// (studio/pdf_import.py's import_pdf), reproduced to the bit for the files PDF producers write. See
// core/src/pdf_import.cpp for what is read and what is declined (NotPorted).
#ifndef KERFKITE_PDF_IMPORT_HPP
#define KERFKITE_PDF_IMPORT_HPP

#include "kerfkite/json.hpp"

namespace kerfkite {

// "pdf-import": {"content": base64 text of the file, "page"} -> the draft with "page", "pageCount" and
// "pageSize". Throws ValueError with the reference's message for a document it refuses.
Json import_pdf(const Json &payload);

}  // namespace kerfkite

#endif
