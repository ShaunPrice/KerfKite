// The assistant's workshop (studio/mcp_workshop.py): Studio's own editor, from a workstation server of this process
// in simulator mode on the loopback interface, in a Chromium-based browser driven over the DevTools protocol. The
// browser may open only that server's pages (the editor, its guides) and fetch nothing elsewhere; its phone pairing
// and serial ports are not reachable. Dialogs are dismissed unless the caller confirms one, downloads become
// artifacts, and the editor's own storage (its libraries and settings) is kept in the output folder between starts.
#ifndef KERFKITE_SERVER_WORKSHOP_HPP
#define KERFKITE_SERVER_WORKSHOP_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "kerfkite/json.hpp"
#include "mcp.hpp"
#include "mcp_tools.hpp"

namespace kerfkite {
namespace server {

struct WorkshopOptions {
    std::string static_dir;    // the editor's files
    std::string step_program;  // the STEP add-on for the workshop's server, or empty
    std::string chromium;      // the browser, or empty to find one
    bool show = false;         // a window to watch, in place of a headless browser
};

class Workshop {
public:
    Workshop(const Artifacts &artifacts, WorkshopOptions options);
    ~Workshop();

    // Each starts the workshop the first time, under the workshop's lock, as the tools of the same name did.
    Json state();
    Json controls(const std::string &query, long long offset, long long limit, bool visible_only);
    Json action(const std::string &action, const std::string &ref, const std::string &value, const Json &points, const Json &confirm);
    Json command(const std::string &line);
    Json macros();
    Json save_macro(const Json &entry, bool replace);
    Json macro(const std::string &name, const Json &parameters);
    // The project, drawing or G-code the editor exports, as an artifact; for stl, the solid's model.
    Json exported(const std::string &kind, Json *model);
    Json text(long long offset, long long length);
    Json print();
    std::string snapshot();

private:
    struct Engine;
    const Artifacts &artifacts_;
    WorkshopOptions options_;
    std::mutex lock_;
    std::unique_ptr<Engine> engine_;

    Engine &started();
};

// The workshop tools: workshop_state, workshop_controls, workshop_action, workshop_command, workshop_macros,
// workshop_save_macro, workshop_macro, workshop_export, workshop_text, workshop_print and workshop_snapshot.
// design_call answers workshop_export of a solid as the design_call tool does.
void add_workshop_tools(mcp::Server &server, Workshop &workshop, std::function<Json(const std::string &, const Json &)> design_call);

}  // namespace server
}  // namespace kerfkite

#endif
