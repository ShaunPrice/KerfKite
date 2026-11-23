// The assistant server's own tools (studio/mcp_server.py, artwork.py, template_catalog.py and the parts of
// mcp_workshop.py that need no browser): the bundled template collection, artwork and template-library files, job
// previews and G-code exports, Studio's design calculations, and the workshop's artifacts. Everything is written
// in one output folder under names the server makes; nothing reaches a machine.
#ifndef KERFKITE_SERVER_MCP_TOOLS_HPP
#define KERFKITE_SERVER_MCP_TOOLS_HPP

#include <string>

#include "kerfkite/json.hpp"
#include "mcp.hpp"

namespace kerfkite {
namespace server {

// The output folder and what is written in it.
class Artifacts {
public:
    explicit Artifacts(const std::string &folder);
    const std::string &root() const { return root_; }
    // ArtworkStore.write: a new file named by a random identifier and the suffix, never over one; its path.
    std::string write(const std::string &suffix, const std::string &content) const;
    // Workshop.create_file(name, content): {"artifact", "name", "size"}, or ValueError.
    Json create_file(const std::string &name, const std::string &content) const;
    // Workshop.file(artifact): the path of an artifact of this folder, or ValueError.
    std::string file(const std::string &artifact) const;

private:
    std::string root_;
};

// A tool as Studio's Python server declared it (mcp_declarations.inc), with the code that runs it.
mcp::Tool declared(const std::string &name, std::function<mcp::Result(const Json &, mcp::Exchange &)> run);

// list_templates, get_template, create_artwork, save_template, preview_job, export_gcode and the artwork guide.
// catalog is the template collection (builtin_templates.kkart). remote: answers name artifacts only, not paths
// on this computer (the HTTP transport).
void add_artwork_tools(mcp::Server &server, const Artifacts &artifacts, const std::string &catalog, bool remote);

// design_actions and design_call: Studio's design routes, called in this process. The answer is design_call's own
// function, for the workshop's solid export.
std::function<Json(const std::string &, const Json &)> add_design_tools(mcp::Server &server, const Artifacts &artifacts, const std::string &step_program);

// workshop_upload and workshop_read_file.
void add_artifact_tools(mcp::Server &server, const Artifacts &artifacts);

// The template collection's file beside the editor's files (studio/static's parent holds it), or given.
std::string find_catalog(const std::string &given, const std::string &static_dir);

}  // namespace server
}  // namespace kerfkite

#endif
