// The controller transports of the workstation server (native/server/machine.cpp) against scripted firmware:
// the cases of tests/test_controller.py, test_grbl_feed_hold.py and test_marlin.py, which they replace. The
// transport loop runs on its own thread as in the server, stepped by the script through MachineOptions::wait
// with a virtual clock, so every timeout is decided by the script and not by the machine's speed.
//
// Nothing here opens a port: the link is a script.
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kerfkite/errors.hpp"
#include "kerfkite/json.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/rotary.hpp"
#include <regex>
#include "machine.hpp"
#include "machine_setup.hpp"
#include "studio.hpp"

using kerfkite::Json;
using kerfkite::ValueError;
using namespace kerfkite::server;

namespace {

int failures = 0, checks = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        ++checks;                                                                           \
        if (!(condition)) {                                                                 \
            ++failures;                                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                                   \
    } while (0)

void check_text(const char *file, int line, const std::string &whole, const std::string &part) {
    ++checks;
    if (whole.find(part) == std::string::npos) {
        ++failures;
        std::fprintf(stderr, "%s:%d: \"%s\" does not contain \"%s\"\n", file, line, whole.c_str(), part.c_str());
    }
}
#define CHECK_TEXT(text, part) check_text(__FILE__, __LINE__, (text), (part))

// The message of what a call throws, or "" when it does not.
template <typename F> std::string refusal(F &&call) {
    try {
        call();
    } catch (const std::exception &e) {
        return e.what();
    }
    return std::string();
}

template <typename F> bool throws_value_error(F &&call) {
    try {
        call();
    } catch (const ValueError &) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

std::string text(const Json &value) { return value.is_string() ? value.get<std::string>() : std::string(); }

// A controller's side of the link. Grbl's script holds one reply that every read returns until the next step,
// as the reference's Mock did; Marlin's script queues replies that reads consume, as its FakeSerial did.
class ScriptedLink : public Link {
public:
    explicit ScriptedLink(bool sticky) : sticky_(sticky) {}
    void write(const std::string &bytes) override {
        std::lock_guard<std::mutex> hold(m_);
        writes_.push_back(bytes);
    }
    std::string readline(size_t) override {
        std::lock_guard<std::mutex> hold(m_);
        if (sticky_) return current_.empty() ? std::string() : current_ + "\n";
        if (queue_.empty()) return std::string();
        std::string next = queue_.front();
        queue_.pop_front();
        return next;
    }
    void close() override {}
    void reply(const std::string &line) {
        std::lock_guard<std::mutex> hold(m_);
        if (sticky_) current_ = line;
        else if (!line.empty()) queue_.push_back(line);
    }
    std::vector<std::string> writes() {
        std::lock_guard<std::mutex> hold(m_);
        return writes_;
    }

private:
    bool sticky_;
    std::mutex m_;
    std::string current_;
    std::deque<std::string> queue_;
    std::vector<std::string> writes_;
};

struct Step {
    Step(double e, std::string r, std::function<void()> a = std::function<void()>()) : elapsed(e), reply(std::move(r)), action(std::move(a)) {}
    double elapsed;
    std::string reply;
    std::function<void()> action;  // before the loop reads the reply, as the reference's stepped wait ran it
};

// A transport on a scripted link, stepped by the script.
class Rig {
public:
    explicit Rig(const std::string &protocol, double timeout = 15) {
        const bool marlin = protocol == "marlin";
        options_.open = [this, marlin](const std::string &) {
            auto link = std::make_unique<ScriptedLink>(!marlin);
            link_ = link.get();
            return std::unique_ptr<Link>(std::move(link));
        };
        options_.ports = [] { return std::vector<PortInfo>{{"FAKE", "scripted firmware"}}; };
        options_.clock = [this] { return now_.load(); };
        options_.wait = [this] { return step(); };
        options_.timeout = timeout;
        machine = marlin ? marlin_machine(options_, 0) : grbl_machine(options_, {});
    }
    ~Rig() { machine.reset(); }

    // Connects and runs the steps on the transport's loop; returns when the script is spent.
    void run(std::vector<Step> steps) {
        {
            std::lock_guard<std::mutex> hold(m_);
            steps_ = std::move(steps);
            next_ = 0;
            done_ = false;
        }
        machine->connect("FAKE");
        std::unique_lock<std::mutex> hold(m_);
        if (!spent_.wait_for(hold, std::chrono::seconds(30), [this] { return done_; })) {
            std::fprintf(stderr, "the script did not finish\n");
            ++failures;
        }
    }

    std::vector<std::string> writes() { return link_ ? link_->writes() : std::vector<std::string>(); }
    size_t count(const std::string &bytes) {
        size_t n = 0;
        for (const std::string &w : writes()) n += w == bytes;
        return n;
    }
    Json status() { return machine->status(); }

    std::unique_ptr<Machine> machine;

private:
    bool step() {
        std::function<void()> action;
        {
            std::lock_guard<std::mutex> hold(m_);
            if (done_) return true;
            if (next_ >= steps_.size()) {
                done_ = true;
                spent_.notify_all();
                return true;
            }
            const Step &s = steps_[next_++];
            now_ = now_.load() + s.elapsed;
            if (link_) link_->reply(s.reply);
            action = s.action;
        }
        if (action) {
            try {
                action();
            } catch (const std::exception &e) {
                // A step of the script that the transport refused: the loop goes on, the test fails.
                std::fprintf(stderr, "a scripted step was refused: %s\n", e.what());
                ++failures;
            }
        }
        return false;
    }

    MachineOptions options_;
    ScriptedLink *link_ = nullptr;
    std::atomic<double> now_{10.0};
    std::mutex m_;
    std::condition_variable spent_;
    std::vector<Step> steps_;
    size_t next_ = 0;
    bool done_ = false;
};

// ---- Grbl: test_grbl_feed_hold.py ---------------------------------------------------------------------------

std::vector<std::string> errors_seen;

std::function<void()> command(Rig &rig, const char *action) {
    return [&rig, action] {
        const std::string message = refusal([&] { rig.machine->command(action, Json::object()); });
        if (!message.empty()) errors_seen.push_back(std::string(action) + ": " + message);
    };
}

// FeedHoldTests.run_serial: the boot that makes the controller ready, a job started on Run, then the steps.
void run_serial(Rig &rig, std::vector<Step> steps, std::vector<std::string> lines = {"M5"}) {
    std::vector<Step> all = {{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "ok"}, {0.1, "<Idle>"},
                             {0.1, "<Run>", [&rig, lines] {
                                  const std::string message = refusal([&] { rig.machine->start(lines); });
                                  if (!message.empty()) errors_seen.push_back("start: " + message);
                              }}};
    all.insert(all.end(), steps.begin(), steps.end());
    rig.run(std::move(all));
}

std::vector<Step> repeat(int n, Step step) { return std::vector<Step>(static_cast<size_t>(n), step); }

std::vector<Step> join(std::initializer_list<std::vector<Step>> parts) {
    std::vector<Step> out;
    for (const auto &part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

void finished(Rig &rig) {
    const Json state = rig.status();
    CHECK(state.at("error").is_null());
    CHECK(text(state.at("state")) == "Idle");
    CHECK(state.at("progress") == Json(100));
    CHECK(state.at("active") == Json(false));
    CHECK(rig.count("\x18") == 0);
}

void feed_hold() {
    {
        // A pending acknowledgement survives a confirmed hold and the job resumes.
        Rig rig("grbl");
        run_serial(rig, join({{{1, "<Hold:1>", command(rig, "hold")}, {1, "<Hold:0>"}}, repeat(20, {1, "<Hold:0>"}),
                              {{0.1, "<Run>", command(rig, "resume")}, {1, "ok"}, {1, "<Idle>"}}}));
        finished(rig);
        CHECK(rig.count("M5\n") == 1);
    }
    {
        // A drain before a long hold waits for Idle.
        Rig rig("grbl");
        run_serial(rig, join({{{1, "ok"}, {1, "<Hold:0>", command(rig, "hold")}}, repeat(125, {1, "<Hold:0>"}),
                              {{0.1, "<Run>", command(rig, "resume")}, {1, "<Idle>"}}}));
        finished(rig);
    }
    {
        // An acknowledgement during a long hold starts a new drain interval.
        Rig rig("grbl");
        run_serial(rig, join({{{1, "<Hold:0>", command(rig, "hold")}}, repeat(20, {1, "<Hold:0>"}), {{1, "ok"}}, repeat(125, {1, "<Hold:0>"}),
                              {{0.1, "<Run>", command(rig, "resume")}}, repeat(119, {1, "<Run>"}), {{0.1, "<Idle>"}}}));
        finished(rig);
    }
    {
        // Resuming keeps only what was left of the completion interval.
        Rig rig("grbl");
        run_serial(rig, join({{{1, "ok"}}, repeat(110, {1, "<Run>"}), {{1, "<Hold:0>", command(rig, "hold")}}, repeat(125, {1, "<Hold:0>"}),
                              {{0.1, "<Run>", command(rig, "resume")}}, repeat(10, {1, "<Run>"})}));
        CHECK_TEXT(text(rig.status().at("error")), "Completion timeout");
        CHECK(rig.count("\x18") > 0);
    }
    {
        // Resuming keeps only what was left of the acknowledgement interval.
        Rig rig("grbl");
        run_serial(rig, join({repeat(10, {1, "<Run>"}), {{1, "<Hold:0>", command(rig, "hold")}}, repeat(18, {1, "<Hold:0>"}),
                              {{0.1, "<Run>", command(rig, "resume")}}, repeat(5, {1, "<Run>"})}));
        CHECK_TEXT(text(rig.status().at("error")), "Acknowledgement timeout");
        CHECK(rig.count("\x18") > 0);
    }
    {
        // A late hold cannot mask a pending acknowledgement's timeout.
        Rig rig("grbl");
        run_serial(rig, join({{{1, "<Hold:1>", command(rig, "hold")}}, repeat(14, {1, "<Hold:1>"}), {{1, "<Hold:0>"}}}));
        CHECK_TEXT(text(rig.status().at("error")), "Acknowledgement timeout");
        CHECK(rig.count("~") == 0);
    }
    {
        // An unconfirmed hold times out with no acknowledgement pending.
        Rig rig("grbl");
        run_serial(rig, join({{{0.1, "ok", command(rig, "hold")}}, repeat(16, {1, "<Run>"})}), {"M5", "M5"});
        CHECK_TEXT(text(rig.status().at("error")), "Feed hold was not confirmed");
        CHECK(rig.count("M5\n") == 1);
    }
    {
        // A stale link during a hold rejects a late status.
        Rig rig("grbl");
        run_serial(rig, {{1, "<Hold:0>", command(rig, "hold")}, {4, "<Hold:0>"}});
        CHECK_TEXT(text(rig.status().at("error")), "Status timeout");
        CHECK(rig.count("\x18") > 0);
    }
    {
        // A decelerating controller cannot be resumed, and queued motion waits for Run.
        Rig rig("grbl");
        std::vector<std::string> observed;
        std::vector<size_t> sent;
        const auto cannot_resume = [&] {
            observed.push_back(text(rig.status().at("state")));
            observed.push_back(refusal([&] { rig.machine->command("resume", Json::object()); }));
        };
        const auto capture = [&] {
            observed.push_back(text(rig.status().at("state")));
            sent.push_back(rig.count("M5\n"));
        };
        run_serial(rig,
                   {{0.1, "<Hold:1>", command(rig, "hold")},
                    {0.1, "ok", cannot_resume},
                    {0.1, "<Hold:0>"},
                    {0.1, "<Hold:0>", command(rig, "resume")},
                    {0.1, "<Hold:0>", capture},
                    {0.1, "<Run>", capture},
                    {0.1, "ok"},
                    {0.1, "<Idle>"}},
                   {"M5", "M5"});
        CHECK(observed.size() == 4);
        if (observed.size() == 4) {
            CHECK(observed[0] == "Decelerating");
            CHECK_TEXT(observed[1], "fully stopped");
            CHECK(observed[2] == "Resuming" && observed[3] == "Resuming");
        }
        CHECK(sent == std::vector<size_t>({1, 1}));  // the second line is still queued at both points
        CHECK(rig.count("~") == 1);
        finished(rig);
    }
    {
        // An unacknowledged resume cannot send the next line.
        Rig rig("grbl");
        run_serial(rig, join({{{0.1, "<Hold:0>", command(rig, "hold")}, {0.1, "ok"}, {0.1, "<Hold:0>", command(rig, "resume")}},
                              repeat(16, {1, "<Hold:0>"})}),
                   {"M5", "M5"});
        CHECK_TEXT(text(rig.status().at("error")), "Resume was not confirmed");
        CHECK(rig.count("M5\n") == 1);
    }
    {
        // A feed hold while idle clears before the next job.
        Rig rig("grbl");
        std::vector<std::string> seen;
        const auto hold_idle = [&] {
            rig.machine->command("hold", Json::object());
            seen.push_back(text(rig.status().at("state")));
            seen.push_back(refusal([&] { rig.machine->start({"M5"}); }));
        };
        const auto start_again = [&] {
            seen.push_back(refusal([&] { rig.machine->start({"M5"}); }));
            seen.push_back(rig.status().at("active") == Json(true) ? "active" : "not active");
        };
        rig.run({{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "ok"}, {0.1, "<Idle>"}, {0.1, "<Idle>", hold_idle}, {0.1, "<Idle>", start_again}});
        CHECK(seen.size() == 4);
        if (seen.size() == 4) {
            CHECK(seen[0] == "Hold requested");
            CHECK_TEXT(seen[1], "idle");
            CHECK(seen[2].empty());
            CHECK(seen[3] == "active");
        }
    }
    {
        // A stale resume writes no cycle start.
        Rig rig("grbl");
        std::vector<std::string> results;
        const auto try_resume = [&] {
            results.push_back(refusal([&] { rig.machine->command("resume", Json::object()); }));
            results.push_back(text(rig.status().at("error")));
        };
        run_serial(rig, {{1, "<Hold:0>", command(rig, "hold")}, {4, "<Hold:0>", try_resume}});
        CHECK(results.size() == 2 && !results[0].empty());
        if (results.size() == 2) CHECK_TEXT(results[1], "Status timeout");
        CHECK(rig.count("~") == 0);
    }
    {
        // An alarm during a confirmed hold aborts.
        Rig rig("grbl");
        run_serial(rig, {{1, "<Hold:0>", command(rig, "hold")}, {1, "ALARM:1"}});
        CHECK(text(rig.status().at("error")) == "ALARM:1");
        CHECK(text(rig.status().at("state")) == "Fault");
        CHECK(rig.count("\x18") > 0);
    }
}

// ---- Grbl: test_controller.py -------------------------------------------------------------------------------

bool wait_until(const std::function<bool()> &done, double seconds = 5) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

Json jog(const char *axis, double distance, double feed) {
    Json fields = Json::object();
    fields["axis"] = axis;
    fields["distance"] = distance;
    fields["feed"] = feed;
    return fields;
}

void controller() {
    {
        // The simulator's life: refused before connecting, held, reset, run to completion.
        std::unique_ptr<Machine> c = grbl_machine(MachineOptions(), {});
        CHECK(!refusal([&] { c->start({"M5"}); }).empty());
        c->connect("simulator");
        c->start(std::vector<std::string>(100, "M5"));
        CHECK(!refusal([&] { c->command("zero", Json::object()); }).empty());
        c->command("hold", Json::object());
        const Json before = c->status().at("progress");
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK(c->status().at("progress") == before);
        c->command("reset", Json::object());
        CHECK(c->status().at("active") == Json(false));
        c->start({"M5"});
        CHECK(wait_until([&] { return c->status().at("active") == Json(false); }));
        CHECK(c->status().at("progress") == Json(100));
        CHECK(text(c->status().at("state")) == "Idle");
        // Jogs outside the pendant's bounds are refused before anything moves.
        for (const Json &fields : {jog("A", 1, 100), jog("X", std::numeric_limits<double>::quiet_NaN(), 100), jog("Y", 11, 100), jog("Z", 1, 1001)})
            CHECK(throws_value_error([&] { c->command("jog", fields); }));
        c->disconnect();
    }
    {
        // An acknowledgement is not completion: messages decide it in their order, on one clock tick.
        Rig rig("grbl");
        std::vector<Json> states;
        const auto look = [&] { states.push_back(rig.status()); };
        rig.run({{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "ok"}, {0.1, "<Idle>"}, {0.1, "", [&] { rig.machine->start({"M5"}); }},
                 {0, "", look},  // the line has gone out
                 {0, "<Idle|MPos:1,2,3>", look},
                 {0, "ok", look},
                 {0, "<Run|MPos:1,2,3>", look},
                 {0, "<Idle|MPos:1,2,3>", look},
                 {0, ""}});
        // Each look sees the state after the reply of the step before it.
        CHECK(states.size() == 5);
        if (states.size() == 5) {
            CHECK(states[2].at("active") == Json(true));  // Idle before the acknowledgement
            CHECK(states[3].at("active") == Json(true));  // the acknowledgement
            CHECK(states[3].at("progress").get<double>() < 100);
            CHECK(states[4].at("active") == Json(true));  // Run
        }
        const Json state = rig.status();
        CHECK(state.at("active") == Json(false));
        CHECK(state.at("progress") == Json(100));
        CHECK(state.at("position").is_object() && text(state.at("position").at("basis")) == "MPos");
    }
    {
        // An error discards the queue and latches the fault.
        Rig rig("grbl");
        std::string later;
        rig.run({{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "ok"}, {0.1, "<Idle|MPos:0,0,0>"},
                 {0, "", [&] { rig.machine->start({"M5", "G1 X10"}); }},
                 {0, "error:20"},
                 {0, "<Idle|MPos:0,0,0>", [&] { later = refusal([&] { rig.machine->start({"M5"}); }); }}});
        CHECK(rig.status().at("active") == Json(false));
        CHECK(rig.count("G1 X10\n") == 0);
        CHECK(!later.empty());
    }
    {
        // A reset banner during a job aborts it.
        Rig rig("grbl");
        rig.run({{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "ok"}, {0.1, "<Idle>"}, {0, "", [&] { rig.machine->start({"M5"}); }},
                 {0, "Grbl 1.1h [help]"}});
        CHECK(!rig.status().at("error").is_null());
        CHECK(rig.status().at("active") == Json(false));
    }
    {
        // Ready needs the settings, the end of their readback and a status report.
        Rig rig("grbl");
        std::vector<Json> ready;
        const auto look = [&] { ready.push_back(rig.status().at("ready")); };
        rig.run({{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "<Idle|MPos:0,0,0>", look}, {0.1, "ok"}, {0.1, "<Idle|MPos:0,0,0>", look},
                 {0.1, ""}});
        CHECK(ready.size() == 2);
        if (ready.size() == 2) CHECK(ready[0] == Json(false));
        CHECK(rig.status().at("ready") == Json(true));
    }
}

// ---- Marlin: test_marlin.py --------------------------------------------------------------------------------

std::string frame(long number, const std::string &line) {
    const std::string payload = "N" + std::to_string(number) + " " + line;
    unsigned checksum = 0;
    for (const unsigned char c : payload) checksum ^= c;
    return payload + "*" + std::to_string(checksum) + "\n";
}

// The handshake that makes a scripted Marlin ready: M110, then the three reports, each acknowledged.
std::vector<Step> marlin_ready() { return {{0.1, ""}, {0.1, "ok\n"}, {0.1, "ok\n"}, {0.1, "ok\n"}, {0.1, "ok\n"}, {0.1, ""}}; }

void marlin() {
    {
        // Lines are numbered and checksummed, one outstanding at a time; the reports come first.
        Rig rig("marlin");
        size_t outstanding = 0;
        rig.run(join({marlin_ready(), {{0.1, "", [&] { rig.machine->start({"G1 X10", "M5"}); }}, {0.1, "", [&] { outstanding = rig.writes().size(); }},
                                       {0.1, ""}, {0.1, "ok N4 P15 B3\n"}, {0.1, ""}}}));
        const std::vector<std::string> w = rig.writes();
        CHECK(w.size() >= 6);
        if (w.size() >= 6) {
            CHECK(w[0] == frame(0, "M110 N0"));
            CHECK(w[1] == frame(1, "M115") && w[2] == frame(2, "M211") && w[3] == frame(3, "M503"));
            CHECK(w[4] == frame(4, "G1 X10"));
            CHECK(w[5] == frame(5, "M5"));
        }
        CHECK(outstanding == 5);  // nothing more until the line is acknowledged
    }
    {
        // Completion needs M400's acknowledgement.
        Rig rig("marlin");
        std::vector<Json> seen;
        const auto look = [&] { seen.push_back(rig.status()); };
        rig.run(join({marlin_ready(), {{0.1, "", [&] { rig.machine->start({"G1 X10"}); }}, {0.1, "ok\n"}, {0.1, "", look}, {0.1, "ok\n"}, {0.1, ""}}}));
        CHECK(seen.size() == 1);
        if (seen.size() == 1) {
            CHECK(seen[0].at("active") == Json(true));
            CHECK(seen[0].at("progress") == Json(99));
        }
        const std::vector<std::string> w = rig.writes();
        CHECK(!w.empty() && w.back() == frame(5, "M400"));
        CHECK(rig.status().at("progress") == Json(100));
        CHECK(rig.status().at("active") == Json(false));
    }
    {
        // A buffered acknowledgement during a host pause does not complete the job before the fence.
        Rig rig("marlin");
        std::vector<Json> seen;
        size_t written = 0;
        const auto look = [&] {
            seen.push_back(rig.status());
            written = rig.writes().size();
        };
        rig.run(join({marlin_ready(),
                      {{0.1, "", [&] { rig.machine->start({"G1 X10"}); }},
                       {0.1, "", command(rig, "hold")},
                       {0.1, "ok\n"},
                       {0.1, "", look},
                       {0.1, "", look},
                       {0.1, "", command(rig, "resume")},
                       {0.1, "ok\n"},
                       {0.1, ""}}}));
        CHECK(seen.size() == 2);
        if (seen.size() == 2) {
            CHECK(seen[1].at("active") == Json(true));
            CHECK(text(seen[1].at("state")) == "Hold");
            CHECK(seen[1].at("progress") == Json(99));
        }
        CHECK(written == 5);  // the job's line alone: the fence waits for the resume
        CHECK(rig.status().at("active") == Json(false));
        CHECK(rig.status().at("progress") == Json(100));
    }
    {
        // A resend consumes the handshake's ok, not the job's acknowledgement.
        Rig rig("marlin");
        std::vector<Json> seen;
        rig.run(join({marlin_ready(), {{0.1, "", [&] { rig.machine->start({"G1 X10"}); }},
                                       {0.1, "Error:checksum mismatch, Last Line: 3\n"},
                                       {0.1, "Resend: 4\n"},
                                       {0.1, "ok\n"},
                                       {0.1, "", [&] { seen.push_back(rig.status()); }},
                                       {0.1, "ok N4\n"},
                                       {0.1, ""}}}));
        CHECK(rig.count(frame(4, "G1 X10")) == 2);
        CHECK(seen.size() == 1 && seen[0].at("progress") == Json(0));
        CHECK(rig.status().at("error").is_null());
    }
    {
        // Resends are bounded and never replay acknowledged motion.
        Rig rig("marlin");
        std::vector<Step> steps = join({marlin_ready(), {{0.1, "", [&] { rig.machine->start({"G1 X1"}); }}, {0.1, ""}}});
        for (int i = 0; i < 3; ++i) {
            steps.push_back({0.1, "rs N4\n"});
            steps.push_back({0.1, "ok\n"});
        }
        steps.push_back({0.1, "Resend: 4\n"});
        steps.push_back({0.1, ""});
        rig.run(steps);
        CHECK(text(rig.status().at("state")) == "Fault");
        CHECK(!rig.writes().empty() && rig.writes().back() == "M112\n");
        Rig old("marlin");
        old.run(join({marlin_ready(), {{0.1, "", [&] { old.machine->start({"G1 X1"}); }}, {0.1, ""}, {0.1, "Resend: 3\n"}, {0.1, ""}}}));
        CHECK(text(old.status().at("state")) == "Fault");
        CHECK_TEXT(text(old.status().at("error")), "refusing to replay");
    }
    {
        // Commands are Marlin's, and a reset needs a reconnect.
        Rig rig("marlin");
        std::vector<std::string> found;
        const auto jog_x = [&] { rig.machine->command("jog", jog("X", 1, 100)); };
        rig.run(join({marlin_ready(), {{0.1, "", jog_x}}, repeat(5, {0.1, "ok\n"}), {{0.1, ""}}}));
        std::vector<std::string> sent;
        for (const std::string &w : rig.writes()) sent.push_back(w);
        CHECK(sent.size() >= 9);
        if (sent.size() >= 9) {
            CHECK(sent[4] == frame(4, "G21") && sent[5] == frame(5, "G91") && sent[6] == frame(6, "G1 X1.000 F100.0"));
            CHECK(sent[7] == frame(7, "G90") && sent[8] == frame(8, "M400"));
        }
        Rig paused("marlin");
        std::vector<std::string> states;
        size_t written = 0;
        paused.run(join({marlin_ready(), {{0.1, "", [&] {
                                               paused.machine->start({"G1 X1"});
                                               paused.machine->command("hold", Json::object());
                                               states.push_back(text(paused.status().at("state")));
                                               written = paused.writes().size();
                                               paused.machine->command("resume", Json::object());
                                               states.push_back(text(paused.status().at("state")));
                                               paused.machine->command("reset", Json::object());
                                               found.push_back(refusal([&] { paused.machine->start({"M5"}); }));
                                           }},
                                          {0.1, ""}}}));
        CHECK(states == std::vector<std::string>({"Hold", "Run"}));
        CHECK(written == 4);  // a host pause writes nothing
        CHECK(!paused.writes().empty() && paused.writes().back() == "M112\n");
        CHECK(found.size() == 1 && !found[0].empty());
        for (const auto &pair : std::vector<std::pair<const char *, const char *>>{{"home", "G28"}, {"zero", "G92 X0 Y0 Z0"}}) {
            Rig one("marlin");
            one.run(join({marlin_ready(), {{0.1, "", [&] { one.machine->command(pair.first, Json::object()); }}, {0.1, ""}}}));
            CHECK(one.writes().size() >= 5 && one.writes()[4] == frame(4, pair.second));
        }
    }
    {
        // Invalid jobs and jogs are refused, among them lines that renumber the link.
        Rig rig("marlin");
        std::vector<bool> refused;
        rig.run(join({marlin_ready(), {{0.1, "", [&] {
                                            for (const std::vector<std::string> &lines :
                                                 std::vector<std::vector<std::string>>{{},
                                                                                       {"M5\nG1 X1"},
                                                                                       {"N1 G1 X1*33"},
                                                                                       {"M110 N0"},
                                                                                       {"M110N0"},
                                                                                       {"G1 X1\x18"},
                                                                                       {"; only comment"},
                                                                                       {"(comment)"},
                                                                                       std::vector<std::string>(200001, "G1 X1")})
                                                refused.push_back(throws_value_error([&] { rig.machine->start(lines); }));
                                            for (const Json &fields :
                                                 {jog("A", 1, 100), jog("X", std::numeric_limits<double>::quiet_NaN(), 100), jog("Z", 11, 100)})
                                                refused.push_back(throws_value_error([&] { rig.machine->command("jog", fields); }));
                                        }},
                                       {0.1, ""}}}));
        CHECK(refused.size() == 12);
        for (bool r : refused) CHECK(r);
    }
    {
        // A reply that arrives in pieces is read whole; a paused outstanding command still times out.
        Rig rig("marlin", 60);
        rig.run(join({{{0.1, ""}, {0.1, "o"}, {0.1, "k\n"}, {0.1, "ok\n"}, {0.1, "ok\n"}, {0.1, "ok\n"}, {0.1, ""}},
                      {{0.1, "", [&] { rig.machine->start({"G1 X1"}); }}, {0.1, "", command(rig, "hold")}},
                      {{61, ""}, {0.1, ""}}}));
        CHECK(text(rig.status().at("state")) == "Fault");
        CHECK(!rig.writes().empty() && rig.writes().back() == "M112\n");
        CHECK(rig.status().at("ready") == Json(false));
    }
    {
        // The simulator: a host pause stops progress, a resume completes the job, a reset needs a reconnect.
        std::unique_ptr<Machine> c = marlin_machine(MachineOptions(), 0);
        c->connect("simulator");
        c->start(std::vector<std::string>(20, "G1 X1"));
        c->command("hold", Json::object());
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK(c->status().at("progress").get<double>() < 100);
        c->command("resume", Json::object());
        CHECK(wait_until([&] { return c->status().at("active") == Json(false); }));
        CHECK(c->status().at("progress") == Json(100));
        CHECK(c->status().at("position").is_null());
        c->command("reset", Json::object());
        CHECK(!refusal([&] { c->start({"M5"}); }).empty());
        c->disconnect();
    }
    for (const char *reply : {"start\n", "ok N999\n", "Error:Printer halted. kill() called!\n"}) {
        // An unexpected restart or a wrong acknowledgement aborts the job.
        Rig rig("marlin");
        rig.run(join({marlin_ready(), {{0.1, "", [&] { rig.machine->start({"M5"}); }}, {0.1, ""}, {0.1, reply}, {0.1, ""}}}));
        CHECK(text(rig.status().at("state")) == "Fault");
        CHECK(rig.status().at("active") == Json(false));
    }
}

// ---- what the controller reports about its machine: test_machine_setup.py -----------------------------------

Json grbl_settings() {
    return Json::parse(R"({"13": 0, "20": 1, "21": 0, "22": 1, "30": 12000, "31": 0, "32": 0, "110": 5000, "111": 4000,
                           "112": 600, "130": 300, "131": 180, "132": 45})");
}

Json grbl_status(Json changes = Json::object()) {
    Json status = Json::parse(R"({"connected": true, "simulated": false, "error": null, "ready": true, "protocol": "grbl",
                                  "position_count": 3, "banner": "Grbl 1.1h ['$' for help]",
                                  "identification": ["[VER:1.1h.20190825:]", "[OPT:V,15,128]"]})");
    status["settings"] = grbl_settings();
    for (const auto &item : changes.items()) status[std::string(item.key())] = item.value();
    return status;
}

bool any_contains(const Json &list, const std::string &part) {
    for (const Json &item : list)
        if (text(item).find(part) != std::string::npos) return true;
    return false;
}

void describe() {
    {
        const Json machine = describe_machine(grbl_status());
        CHECK(machine.at("available") == Json(true));
        CHECK(text(machine.at("firmware")) == "Grbl 1.1h.20190825");
        CHECK(text(machine.at("name")) == "Grbl 1.1h.20190825 \xc2\xb7 300 \xc3\x97 180 mm");
        CHECK(text(machine.at("identity")) == "grbl|Grbl 1.1h.20190825|300|180|45");
        CHECK(machine.at("environment") == Json::parse(R"({"protocol": "grbl", "width": 300, "height": 180, "z_travel": 45, "feed_max": 4000,
                                                          "plunge_max": 600, "power_max": 12000, "laser_mode": false})"));
        CHECK(machine.at("homing") == Json(true) && machine.at("soft_limits") == Json(true) && machine.at("hard_limits") == Json(false));
        CHECK(machine.at("missing").empty() && machine.at("warnings").empty());
        CHECK(any_contains(machine.at("reported"), "Z travel 45 mm"));
    }
    {
        // What the controller does not report is listed, not invented.
        Json changes = Json::object();
        changes["settings"] = Json::parse(R"({"30": 1000, "32": 1})");
        changes["identification"] = Json::array();
        changes["banner"] = Json();
        const Json machine = describe_machine(grbl_status(changes));
        CHECK(machine.at("available") == Json(true));
        CHECK(machine.at("environment") == Json::parse(R"({"protocol": "grbl", "power_max": 1000, "laser_mode": true})"));
        CHECK(machine.at("missing") == Json::parse(R"(["X travel", "Y travel", "Z travel", "maximum feed"])"));
        CHECK(machine.at("name").is_null() && machine.at("identity").is_null() && machine.at("firmware").is_null());
    }
    {
        // Values a workspace cannot take are not applied, and the warnings say why.
        Json settings = grbl_settings();
        settings["130"] = 2500;
        settings["110"] = 30000;
        settings["111"] = 25000;
        settings["13"] = 1;
        settings["132"] = std::numeric_limits<double>::quiet_NaN();
        Json changes = Json::object();
        changes["settings"] = settings;
        const Json machine = describe_machine(grbl_status(changes));
        CHECK(!machine.at("environment").contains("width"));
        CHECK(machine.at("environment").at("feed_max") == Json(10000));
        CHECK(machine.at("name").is_null());
        CHECK(any_contains(machine.at("missing"), "Z travel"));
        CHECK(any_contains(machine.at("warnings"), "beyond the 2000 mm"));
        CHECK(any_contains(machine.at("warnings"), "inches"));
        Json tall_settings = grbl_settings();
        tall_settings["132"] = 2400;
        Json tall_changes = Json::object();
        tall_changes["settings"] = tall_settings;
        const Json tall = describe_machine(grbl_status(tall_changes));
        CHECK(!tall.at("environment").contains("z_travel"));
        CHECK(!any_contains(tall.at("missing"), "Z travel"));
        CHECK(any_contains(tall.at("warnings"), "Z travel of 2400 mm is beyond"));
    }
    {
        // The firmware is recognised, and a dialect that does not match it is pointed out.
        const struct {
            const char *changes, *firmware, *detected;
        } cases[] = {
            {R"({"identification": ["[VER:1.1f.20230414:]", "[FIRMWARE:grblHAL]"], "protocol": "grblhal"})", "grblHAL 1.1f.20230414", "grblhal"},
            {R"({"identification": ["[VER:3.7.8 FluidNC v3.7.8:]"], "banner": null, "protocol": "fluidnc"})", "3.7.8 FluidNC v3.7.8", "fluidnc"},
            {R"({"identification": [], "banner": "FluidNC v3.9.1 [$ for help]", "protocol": "fluidnc"})", "FluidNC v3.9.1", "fluidnc"},
            {R"({"identification": ["[VER:1.3a.20210424:]", "[MSG:Using machine:CNC_xPRO_V5_XYAZ_NC]"], "protocol": "xpro-v5"})",
             "Grbl_Esp32 1.3a.20210424 \xc2\xb7 xPRO V5", "xpro-v5"},
            {R"({"identification": [], "banner": "Grbl 0.9j [$ for help]"})", "Grbl 0.9j", "grbl"},
        };
        for (const auto &c : cases) {
            const Json machine = describe_machine(grbl_status(Json::parse(c.changes)));
            CHECK(text(machine.at("firmware")) == c.firmware);
            CHECK(text(machine.at("detected_protocol")) == c.detected);
            CHECK(machine.at("warnings").empty());
        }
        const Json wrong = describe_machine(grbl_status(Json::parse(R"({"identification": ["[VER:1.1f:]", "[FIRMWARE:grblHAL]"]})")));
        CHECK(!wrong.at("warnings").empty() && text(wrong.at("warnings")[0]).find("identifies as grblHAL") != std::string::npos);
        CHECK(text(wrong.at("environment").at("protocol")) == "grbl");  // the connection is what the workspace follows
    }
    {
        // The xPRO's laser power limit comes from its own setting.
        Json settings = grbl_settings();
        settings["32"] = 1;
        settings["Laser/FullPower"] = 255;
        Json changes = Json::object();
        changes["protocol"] = "xpro-v5";
        changes["settings"] = settings;
        CHECK(describe_machine(grbl_status(changes)).at("environment").at("power_max") == Json(255));
    }
    {
        // Marlin's reports.
        const Json readback = Json::parse(R"({"firmware": "Marlin 2.1.2.1", "machine_type": "CNC", "travel": [400.0, 300.0, 80.0],
                                              "max_feed": [18000.0, 18000.0, 300.0], "soft_endstops": true})");
        Json status = Json::parse(R"({"connected": true, "simulated": false, "error": null, "ready": true, "protocol": "marlin"})");
        status["readback"] = readback;
        const Json machine = describe_machine(status);
        CHECK(text(machine.at("name")) == "Marlin 2.1.2.1 \xc2\xb7 CNC \xc2\xb7 400 \xc3\x97 300 mm");
        CHECK(machine.at("environment") ==
              Json::parse(R"({"protocol": "marlin", "width": 400, "height": 300, "z_travel": 80, "feed_max": 10000, "plunge_max": 300})"));
        CHECK(machine.at("missing").empty() && machine.at("warnings").empty() && text(machine.at("detected_protocol")) == "marlin");
        Json loose_readback = readback;
        loose_readback["soft_endstops"] = false;
        Json loose = Json::parse(R"({"connected": true, "ready": true, "protocol": "marlin"})");
        loose["readback"] = loose_readback;
        const Json warned = describe_machine(loose);
        CHECK(!warned.at("warnings").empty() && text(warned.at("warnings")[0]).find("Soft endstops are off") != std::string::npos);
        const Json silent = describe_machine(Json::parse(R"({"connected": true, "ready": true, "protocol": "marlin", "readback": {}})"));
        CHECK(silent.at("missing") == Json::parse(R"(["X travel", "Y travel", "Z travel", "maximum feed"])"));
    }
    {
        // No description without a ready machine.
        const struct {
            Json status;
            const char *reason;
            bool waiting;
        } cases[] = {
            {Json::parse(R"({"connected": false})"), "Connect a controller", false},
            {Json::parse(R"({"connected": true, "simulated": true, "ready": true})"), "simulator has no machine settings", false},
            {grbl_status(Json::parse(R"({"error": "ALARM:1"})")), "reported a fault", false},
            {grbl_status(Json::parse(R"({"ready": false})")), "Waiting for the controller", true},
        };
        for (const auto &c : cases) {
            const Json machine = describe_machine(c.status);
            CHECK(machine.at("available") == Json(false));
            CHECK(machine.at("waiting") == Json(c.waiting));
            CHECK(machine.at("environment") == Json::object());
            CHECK_TEXT(text(machine.at("reason")), c.reason);
        }
    }
}

// A link that answers each written command from a table, as the reference's FakeSerial did.
class TableLink : public Link {
public:
    TableLink(std::vector<std::pair<std::string, std::vector<std::string>>> answers, std::vector<std::string> *writes)
        : answers_(std::move(answers)), writes_(writes) {}
    void write(const std::string &bytes) override {
        std::lock_guard<std::mutex> hold(m_);
        writes_->push_back(bytes);
        for (const auto &answer : answers_)
            if (bytes.find(answer.first) != std::string::npos)
                for (const std::string &line : answer.second) incoming_.push_back(line + "\n");
    }
    std::string readline(size_t) override {
        {
            std::lock_guard<std::mutex> hold(m_);
            if (!incoming_.empty()) {
                std::string line = incoming_.front();
                incoming_.pop_front();
                return line;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return std::string();
    }
    void close() override {}
    void arrive(const std::string &line) {
        std::lock_guard<std::mutex> hold(m_);
        incoming_.push_back(line + "\n");
    }

private:
    std::vector<std::pair<std::string, std::vector<std::string>>> answers_;
    std::vector<std::string> *writes_;
    std::mutex m_;
    std::deque<std::string> incoming_;
};

// Twenty times the wall clock: boot pauses and the report timeout pass quickly, and nothing else is changed.
std::function<double()> fast_clock() {
    const auto start = std::chrono::steady_clock::now();
    return [start] { return 20 * std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
}

MachineOptions table(std::vector<std::pair<std::string, std::vector<std::string>>> answers, std::vector<std::string> *writes,
                     std::vector<std::string> banner = {}) {
    MachineOptions options;
    options.open = [answers, writes, banner](const std::string &) {
        auto link = std::make_unique<TableLink>(answers, writes);
        for (const std::string &line : banner) link->arrive(line);
        return std::unique_ptr<Link>(std::move(link));
    };
    options.ports = [] { return std::vector<PortInfo>{{"fake", "table"}}; };
    options.clock = fast_clock();
    return options;
}

std::string sent_command(const std::string &framed) {
    const size_t space = framed.find(' '), star = framed.find('*');
    return space == std::string::npos ? framed : framed.substr(space + 1, star == std::string::npos ? std::string::npos : star - space - 1);
}

void readback() {
    {
        // Grbl-family controllers are asked to identify themselves, and nothing is written to them.
        std::vector<std::string> settings;
        const Json values = grbl_settings();
        for (const auto &item : values.items())
            settings.push_back("$" + std::string(item.key()) + "=" + item.value().dump());
        settings.push_back("ok");
        std::vector<std::string> writes;
        const MachineOptions options = table({{"$$", settings}, {"$I", {"[VER:1.1h.20190825:]", "[OPT:V,15,128]", "ok"}},
                                              {"?", {"<Idle|MPos:0.000,0.000,0.000|FS:0,0>"}}},
                                             &writes, {"Grbl 1.1h ['$' for help]"});
        Options server;
        server.enable_hardware = true;
        Studio studio(server, options);
        studio.device().connect("fake");
        CHECK(wait_until([&] {
            const Json state = studio.device().status();
            return state.at("ready") == Json(true) && state.at("identification").size() == 2;
        }, 10));
        bool only_reports = true;
        for (const std::string &w : std::vector<std::string>(writes))
            if (!w.empty() && w[0] == '$' && w != "$$\n" && w != "$I\n") only_reports = false;
        CHECK(only_reports);
        CHECK(text(describe_machine(studio.device().status()).at("name")) == "Grbl 1.1h.20190825 \xc2\xb7 300 \xc3\x97 180 mm");
        CHECK(text(studio.device().status().at("banner")) == "Grbl 1.1h ['$' for help]");
        kerfkite::http::Request get;
        get.method = "GET";
        get.path = get.target = "/api/machine";
        get.headers.emplace_back("host", "127.0.0.1");
        CHECK(Json::parse(studio.handle(get).body).at("environment").at("z_travel") == Json(45));
        get.path = get.target = "/api/session";
        CHECK(Json::parse(studio.handle(get).body).at("machine_readback") == Json(true));
        studio.device().disconnect();
    }
    const auto marlin_with = [](std::vector<std::pair<std::string, std::vector<std::string>>> answers, std::vector<std::string> *writes) {
        answers.insert(answers.begin(), {"M110", {"ok"}});
        return marlin_machine(table(answers, writes), 0);
    };
    {
        // Marlin's reports are read at connection; only reports are asked for.
        std::vector<std::string> writes;
        std::unique_ptr<Machine> c = marlin_with(
            {{"M115",
              {"FIRMWARE_NAME:Marlin 2.1.2.1 (Mar  1 2024 10:00:00) SOURCE_CODE_URL:github.com/MarlinFirmware/Marlin PROTOCOL_VERSION:1.0 "
               "MACHINE_TYPE:CNC EXTRUDER_COUNT:0",
               "Cap:SERIAL_XON_XOFF:0", "ok"}},
             {"M211", {"echo:Soft endstops: ON  Min:  X0.00 Y0.00 Z-80.00   Max:  X400.00 Y300.00 Z0.00", "ok"}},
             {"M503", {"echo:; Maximum feedrates (units/s):", "echo:  M203 X300.00 Y300.00 Z5.00", "echo:  M201 X500.00 Y500.00 Z100.00", "ok"}}},
            &writes);
        c->connect("fake");
        CHECK(wait_until([&] { return c->status().at("ready") == Json(true); }, 10));
        const Json expected = Json::parse(R"JSON({"firmware": "Marlin 2.1.2.1 (Mar  1 2024 10:00:00)", "machine_type": "CNC",
                                                  "soft_endstops": true, "travel": [400.0, 300.0, 80.0],
                                                  "max_feed": [18000.0, 18000.0, 300.0]})JSON");
        CHECK(c->status().at("readback") == expected);
        std::vector<std::string> sent;
        for (const std::string &w : std::vector<std::string>(writes)) sent.push_back(sent_command(w));
        CHECK(sent == std::vector<std::string>({"M110 N0", "M115", "M211", "M503"}));  // reports only; nothing is set
        CHECK(describe_machine(c->status()).at("environment").at("width") == Json(400));
        c->disconnect();
    }
    {
        // A Marlin without those reports still connects.
        std::vector<std::string> writes;
        std::unique_ptr<Machine> c = marlin_with({{"M115", {"echo:Unknown command: \"M115\"", "ok"}},
                                                  {"M211", {"Error:M211 is not available", "ok"}},
                                                  {"M503", {std::string(300, 'x'), "ok"}}},
                                                 &writes);
        c->connect("fake");
        CHECK(wait_until([&] { return c->status().at("ready") == Json(true); }, 10));
        const Json state = c->status();
        CHECK(state.at("readback") == Json::object());
        CHECK(state.at("error").is_null());
        CHECK(text(state.at("state")) == "Idle");
        c->disconnect();
    }
    {
        // A Marlin that never answers a report is ready after a short wait, and is asked for nothing more.
        std::vector<std::string> writes;
        std::unique_ptr<Machine> c = marlin_with({}, &writes);
        c->connect("fake");
        CHECK(wait_until([&] { return c->status().at("ready") == Json(true); }, 10));
        const Json state = c->status();
        CHECK(state.at("error").is_null() && text(state.at("state")) == "Idle");
        bool said = false;
        for (const Json &line : state.at("log")) said = said || text(line) == "The firmware did not report its settings";
        CHECK(said);
        CHECK(writes.size() == 2);
        c->disconnect();
    }
    {
        // The simulator reports no machine.
        Studio studio{Options(), MachineOptions()};
        studio.device().connect("simulator");
        const Json machine = describe_machine(studio.device().status());
        CHECK(machine.at("available") == Json(false));
        CHECK_TEXT(text(machine.at("reason")), "simulator has no machine settings");
        studio.device().disconnect();
    }
}

// ---- probe levelling on the simulator and through the server: test_leveling_machine.py ----------------------

Json finished(Machine &c, double seconds = 10) {
    wait_until([&] { return c.status().at("active") == Json(false); }, seconds);
    return c.status();
}

std::unique_ptr<Machine> simulator(std::function<double(double, double)> surface) {
    MachineOptions options;
    options.surface = std::move(surface);
    std::unique_ptr<Machine> c = grbl_machine(options, {});
    c->connect("simulator");
    return c;
}

void simulated_probing() {
    {
        // A probe stops on the surface and reports where.
        auto c = simulator([](double x, double) { return -0.3 + 0.01 * x; });
        c->start({"G0 Z2.000", "G0 X10.000 Y5.000", "G38.2 Z-1.000 F25.0", "G0 Z2.000", "G0 X20.000 Y5.000", "G38.2 Z-1.000 F25.0"});
        const Json status = finished(*c);
        CHECK(text(status.at("state")) == "Idle" && status.at("probe_count") == Json(2) && status.at("error").is_null());
        CHECK(c->probe_reports() == std::vector<std::string>({"[PRB:10.000,5.000,-0.200:1]", "[PRB:20.000,5.000,-0.100:1]"}));
        CHECK(std::fabs(status.at("position").at("z").get<double>() - -0.1) < 1e-7);
        bool logged = false;
        for (const Json &line : status.at("log")) logged = logged || text(line) == "[PRB:20.000,5.000,-0.100:1]";
        CHECK(logged);
    }
    {
        // A probe without contact raises the controller's alarm; a reset clears it.
        auto c = simulator([](double, double) { return -3.0; });
        c->start({"G0 Z2.000", "G38.2 Z-1.000 F25.0", "G0 Z2.000"});
        const Json status = finished(*c);
        CHECK(text(status.at("state")) == "Fault" && text(status.at("error")) == "ALARM:5" && status.at("probe_count") == Json(0));
        c->command("reset", Json::object());
        CHECK(text(c->status().at("state")) == "Idle");
    }
    {
        // Setting work zero moves the offset; reports stay in machine coordinates and belong to one job.
        auto c = simulator([](double, double) { return -0.4; });
        c->start({"G0 Z2.000", "G38.2 Z-1.000 F25.0", "G10 L20 P1 Z0.000", "G0 Z2.000", "G0 X5.000 Y0.000", "G38.2 Z-1.000 F25.0"});
        const Json status = finished(*c);
        CHECK(c->probe_reports() == std::vector<std::string>({"[PRB:0.000,0.000,-0.400:1]", "[PRB:5.000,0.000,-0.400:1]"}));
        CHECK(std::fabs(status.at("position").at("z").get<double>()) < 1e-7);  // the surface is now work Z zero
        c->start({"M5"});
        CHECK(c->probe_reports().empty());
        finished(*c);
        c->command("zero", Json::object());
        finished(*c);
        const Json position = c->status().at("position");
        CHECK(position.at("x") == Json(0.0) && position.at("y") == Json(0.0) && position.at("z") == Json(0.0));
    }
    {
        // A controller's reports are kept for the running job only.
        Rig rig("grbl");
        rig.run({{2.1, ""}, {0.1, "$30=1000"}, {0.1, "$32=0"}, {0.1, "ok"}, {0.1, "<Idle>"},
                 {0.1, "[PRB:0.000,0.000,-1.000:1]"},  // nothing runs: no job's report
                 {0.1, "", [&] { rig.machine->start({"G38.2 Z-1.000 F25.0", "G0 Z2.000"}); }},
                 {0.1, "[PRB:1.000,2.000,-3.456:1]"},
                 {0.1, "ok"},
                 {0.1, ""}});
        CHECK(rig.machine->probe_reports() == std::vector<std::string>({"[PRB:1.000,2.000,-3.456:1]"}));
        CHECK(rig.status().at("probe_count") == Json(1));
        CHECK(rig.status().at("error").is_null());
    }
}

struct Levelling {
    std::unique_ptr<Studio> studio;
    Json project;
    explicit Levelling(std::function<double(double, double)> surface = {}) {
        MachineOptions machine;
        machine.surface = std::move(surface);
        studio = std::make_unique<Studio>(Options(), machine);
        project = Json::parse(R"({"mode": "router", "width": 100, "height": 100, "feed": 600, "power": 1000, "safe": 5, "depth": 0.1,
                                  "step": 0.1, "plunge": 100, "passes": 1, "stock_thickness": 1.6,
                                  "paths": [[[10, 10], [60, 10], [60, 40], [10, 10]]]})");
    }
    kerfkite::http::Response call(const std::string &method, const std::string &path, const Json &body = Json::object()) {
        kerfkite::http::Request r;
        r.method = method;
        r.path = r.target = path;
        r.headers.emplace_back("host", "127.0.0.1");
        if (method == "POST") {
            r.headers.emplace_back("content-type", "application/json");
            r.headers.emplace_back("x-studio-token", studio->token());
            r.body = body.dump();
        }
        return studio->handle(r);
    }
    Json post(const std::string &path, const Json &body = Json::object()) { return Json::parse(call("POST", path, body).body); }
    Json get(const std::string &path) { return Json::parse(call("GET", path).body); }
    Json idle() {
        wait_until([&] { return get("/api/status").at("active") == Json(false); }, 20);
        return get("/api/status");
    }
    Json probe(Json settings = Json::object()) {
        Json body = Json::object();
        body["spacing"] = 30;  // a coarse grid keeps the simulated probing short
        for (const auto &item : settings.items()) body[std::string(item.key())] = item.value();
        body["confirmed"] = true;
        body["project"] = project;
        const kerfkite::http::Response result = call("POST", "/api/level/probe", body);
        CHECK(result.status == 200);
        idle();
        return Json::parse(result.body);
    }
};

double round3(double value) {
    double out = 0;
    kerfkite::text::rounded(value, 3, out);
    return out;
}

void levelled_runs() {
    {
        // Probe, collect and run: the job follows the simulated surface.
        Levelling l;
        const auto surface_z = [](double x, double y) { return 0.0015 * x - 0.001 * y + 0.00001 * std::pow(x - 40, 2); };
        l.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
        const Json started = l.probe(Json::parse(R"({"spacing": 10})"));
        const Json points = started.at("plan").at("points");
        CHECK(started.at("surface").is_null());
        CHECK(l.get("/api/level").at("plan").at("probed") == Json(static_cast<long long>(points.size())));
        const Json surface = l.post("/api/level/collect").at("surface");
        CHECK(surface.at("offsets").size() == points.size());
        CHECK(surface.at("offsets")[0] == Json(0.0));  // the first point became work Z zero
        CHECK(std::fabs(l.get("/api/status").at("position").at("z").get<double>() - 2.0) < 1e-7);  // left at the clearance
        const double zero = surface_z(points[0][0].get<double>(), points[0][1].get<double>());
        // The probing order is a serpentine over the grid's rows.
        const Json grid = started.at("plan").at("grid");
        const long long rows = grid.at("rows").get<long long>(), columns = grid.at("columns").get<long long>();
        std::vector<std::pair<long long, long long>> order;
        for (long long row = 0; row < rows; ++row)
            for (long long k = 0; k < columns; ++k) order.emplace_back(row % 2 ? columns - 1 - k : k, row);
        CHECK(order.size() == points.size());
        bool offsets = order.size() == points.size();
        for (size_t i = 0; offsets && i < points.size(); ++i) {
            const double x = points[i][0].get<double>(), y = points[i][1].get<double>();
            const long long index = order[i].second * surface.at("columns").get<long long>() + order[i].first;
            offsets = std::fabs(surface.at("offsets")[static_cast<size_t>(index)].get<double>() - (round3(surface_z(x, y)) - round3(zero))) < 1e-9;
        }
        CHECK(offsets);
        Json program_request = Json::object();
        program_request["project"] = l.project;
        const Json program = l.post("/api/level/program", program_request).at("lines");
        const size_t plain = kerfkite::generate(l.project).lines.size();
        CHECK(program.size() > plain);
        static const std::regex cut(R"(G1 X(\S+) Y(\S+) F\S+ Z(\S+))");
        size_t cuts = 0;
        bool depths = true;
        for (const Json &line : program) {
            std::smatch m;
            const std::string row = text(line);
            if (!std::regex_match(row, m, cut)) continue;
            ++cuts;
            const double x = std::stod(m[1].str()), y = std::stod(m[2].str()), z = std::stod(m[3].str());
            depths = depths && std::fabs(z - (-0.1 + surface_z(x, y) - zero)) <= 0.004;  // the programmed depth below the surface
        }
        CHECK(cuts > 40);
        CHECK(depths);
        Json run = Json::object();
        run["confirmed"] = true;
        run["project"] = l.project;
        run["leveled"] = true;
        CHECK(l.post("/api/run", run).at("lines") == Json(static_cast<long long>(program.size())));
        const Json status = l.idle();
        CHECK(text(status.at("state")) == "Idle" && status.at("error").is_null() && status.at("progress") == Json(100));
        run.erase("leveled");
        CHECK(l.post("/api/run", run).at("lines") == Json(static_cast<long long>(plain)));
        l.idle();
    }
    {
        // A levelled run needs a probed surface.
        Levelling l;
        l.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
        Json run = Json::object();
        run["confirmed"] = true;
        run["project"] = l.project;
        run["leveled"] = true;
        CHECK(l.call("POST", "/api/run", run).status == 400);
        CHECK_TEXT(text(l.post("/api/run", run).at("error")), "Probe the stock surface");
        CHECK_TEXT(text(l.post("/api/level/collect").at("error")), "Probe the stock surface first");
        run["leveled"] = 1;
        CHECK_TEXT(text(l.post("/api/run", run).at("error")), "true or false");
    }
    {
        // Probing is refused without confirmation, or for another kind of job.
        Levelling l;
        l.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
        const auto refused = [&](Json body) {
            const kerfkite::http::Response r = l.call("POST", "/api/level/probe", body);
            return r.status == 400 ? text(Json::parse(r.body).at("error")) : std::string("status ") + std::to_string(r.status);
        };
        Json body = Json::object();
        body["project"] = l.project;
        CHECK_TEXT(refused(body), "Confirm that the probe is connected");
        body["confirmed"] = true;
        Json laser = l.project;
        laser["mode"] = "laser";
        body["project"] = laser;
        CHECK_TEXT(refused(body), "applies to router jobs");
        body["project"] = l.project;
        body["rotary"] = Json::object();
        CHECK_TEXT(refused(body), "does not apply to rotary");
        body.erase("rotary");
        Json marlin = l.project;
        marlin["protocol"] = "marlin";
        body["project"] = marlin;
        CHECK_TEXT(refused(body), "Grbl-family");
        body["project"] = l.project;
        body["feed"] = 1;
        CHECK_TEXT(refused(body), "within 10 seconds");
        body.erase("feed");
        body["spacing"] = 0.5;
        CHECK_TEXT(refused(body), "Probe spacing must be between");
        CHECK_TEXT(refused(Json::parse(R"({"confirmed": true})")), "Supply a design project");
        CHECK(l.get("/api/status").at("active") == Json(false));
        CHECK(l.get("/api/level").at("plan").is_null());
        l.post("/api/disconnect");
        body.erase("spacing");
        CHECK(l.call("POST", "/api/level/probe", body).status == 400);
    }
    {
        // An incomplete or uneven probing is not accepted.
        bool broken = true;
        Levelling l([&broken](double x, double) { return broken ? (x < 40 ? -0.2 : -5.0) : 0.02 * x; });
        l.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
        Json body = Json::object();
        body["confirmed"] = true;
        body["project"] = l.project;
        l.post("/api/level/probe", body);
        CHECK(text(l.idle().at("error")) == "ALARM:5");
        CHECK_TEXT(text(l.post("/api/level/collect").at("error")), "Probing did not finish: ALARM:5");
        l.post("/api/command", Json::parse(R"({"action": "reset"})"));
        CHECK(l.get("/api/level").at("plan").is_null());  // a reset forgets the plan
        broken = false;
        l.probe();
        CHECK_TEXT(text(l.post("/api/level/collect", Json::parse(R"({"limit": 0.5})")).at("error")), "more than the 0.5 mm limit");
        CHECK(l.get("/api/level").at("surface").is_null());
        CHECK(l.call("POST", "/api/level/collect").status == 200);
    }
    {
        // The surface is forgotten when work zero or the tool may have changed; a jog keeps it.
        const std::vector<std::pair<std::string, Json>> changes = {
            {"/api/command", Json::parse(R"({"action": "zero"})")},
            {"/api/command", Json::parse(R"({"action": "home"})")},
            {"/api/head", Json::parse(R"({"mode": "router", "confirmed": true})")},
            {"/api/level/clear", Json::object()},
            {"/api/disconnect", Json::object()},
            {"/api/connect", Json::parse(R"({"port": "simulator"})")},
        };
        Levelling l;
        for (const auto &change : changes) {
            l.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
            l.probe();
            CHECK(!l.post("/api/level/collect").at("surface").is_null());
            l.post("/api/command", Json::parse(R"({"action": "jog", "axis": "X", "distance": 1, "feed": 100})"));
            l.idle();
            CHECK(!l.get("/api/level").at("surface").is_null());
            CHECK(l.call("POST", change.first, change.second).status == 200);
            const Json state = l.get("/api/level");
            CHECK(state.at("plan").is_null() && state.at("surface").is_null());
            l.idle();
        }
    }
}

// ---- rotary jobs on the controller: test_rotary_stream.py's machine cases -------------------------------------

Json rotary_project(const std::string &post = "grblhal", const std::string &mode = "router") {
    Json p = Json::parse(R"({"operation": "line", "width": 100, "height": 100, "feed": 600, "power": 800, "safe": 3, "depth": 2, "step": 2,
                            "plunge": 100, "passes": 1, "stock_thickness": 4})");
    p["mode"] = mode;
    p["protocol"] = post;
    const double pi = 3.141592653589793;
    p["paths"] = Json::array({Json::array({Json::array({0.0, 5.0}), Json::array({5 * pi, 5.0}), Json::array({10 * pi, 5.0})})});
    return p;
}

Json rotary_options(const std::string &post = "grblhal") {
    Json o = Json::parse(R"({"diameter": 10, "wrap_axis": "X"})");
    o["postprocessor"] = post;
    return o;
}

std::vector<std::string> axes_of(Rig &rig, const std::string &protocol) {
    Json state = rig.status();
    state["protocol"] = protocol;
    return controller_rotary_axes(protocol, state);
}

void rotary_readback() {
    Rig rig("grbl");
    std::vector<std::vector<std::string>> seen;
    std::vector<bool> checks_passed;
    std::vector<Step> steps = {{2.1, ""}, {0, "[FIRMWARE:grblHAL]"}, {0, "[AXS:5:XYZAB]"}, {0, "$376=2"}};
    for (int key : {103, 104, 113, 114, 123, 124, 130, 131, 132, 133, 134}) steps.push_back({0, "$" + std::to_string(key) + "=1000"});
    steps.push_back({0, "<Idle|WPos:0,0,0,0,15>"});
    steps.push_back({0, "", [&] {
                         seen.push_back(axes_of(rig, "grblhal"));
                         Json state = rig.status();
                         state["protocol"] = "grblhal";
                         checks_passed.push_back(state.at("position").at("b") == Json(15));
                         Json options = rotary_options();
                         options["output_axis"] = "B";
                         const Json meta = kerfkite::generate_rotary(rotary_project(), options).at("rotary");
                         checks_passed.push_back(refusal([&] { validate_rotary_machine(meta, state); }).empty());
                         Json tight = state;
                         tight["settings"]["20"] = 1;
                         tight["settings"]["134"] = 100;
                         checks_passed.push_back(throws_value_error([&] { validate_rotary_machine(meta, tight); }));
                     }});
    steps.push_back({0, "<Idle|WPos:0,0,0>"});
    steps.push_back({0, "", [&] { seen.push_back(axes_of(rig, "grblhal")); }});
    steps.push_back({0, "[MSG:Using machine:CNC_xPRO_V5_XYYZ_NC]"});
    steps.push_back({0, "<Idle|WPos:0,0,0,0>"});
    steps.push_back({0, "", [&] { seen.push_back(axes_of(rig, "xpro-v5")); }});
    steps.push_back({0, "[MSG:Using machine:CNC_xPRO_V5_XYAZ_NC]"});
    steps.push_back({0, "", [&] { seen.push_back(axes_of(rig, "xpro-v5")); }});
    steps.push_back({0, "Grbl 1.1"});
    steps.push_back({0, "", [&] { seen.push_back(axes_of(rig, "xpro-v5")); }});
    rig.run(steps);
    const std::vector<std::vector<std::string>> expected = {{"B"}, {}, {}, {"A"}, {}};
    CHECK(seen == expected);
    CHECK(checks_passed == std::vector<bool>({true, true, true}));
}

kerfkite::http::Response route(Studio &studio, const std::string &method, const std::string &path, const Json &body = Json::object()) {
    kerfkite::http::Request r;
    r.method = method;
    r.path = r.target = path;
    r.headers.emplace_back("host", "127.0.0.1");
    if (method == "POST") {
        r.headers.emplace_back("content-type", "application/json");
        r.headers.emplace_back("x-studio-token", studio.token());
        r.body = body.dump();
    }
    return studio.handle(r);
}

std::string route_error(const kerfkite::http::Response &response) {
    try {
        return text(Json::parse(response.body).at("error"));
    } catch (const std::exception &) {
        return std::string();
    }
}

void rotary_machines() {
    {
        // The stock check covers the start of an ascending cut, planar and rotary alike.
        std::vector<std::string> writes;
        const MachineOptions machine = table({{"$I", {"[VER:1.1f.20230414:]", "[FIRMWARE:grblHAL]", "[AXS:4:XYZA]", "ok"}},
                                              {"$376", {"$376=1", "ok"}},
                                              {"$$", {"$30=1000", "$32=0", "$103=50", "$113=720", "$123=90", "$130=500", "$132=100", "ok"}},
                                              {"?", {"<Idle|WPos:0,0,0,0>"}}},
                                             &writes);
        Options server;
        server.enable_hardware = true;
        Studio studio(server, machine);
        CHECK(route(studio, "POST", "/api/connect", Json::parse(R"({"port": "fake", "protocol": "grblhal"})")).status == 200);
        CHECK(wait_until([&] { return studio.device().status().at("ready") == Json(true); }, 10));
        Json p = rotary_project();
        p["paths"] = Json::parse("[[[0, 5], [10, 5]]]");
        p["path_depths"] = Json::parse("[[-4, -1]]");
        p["depth"] = 4;
        p["step"] = 4;
        p["stock_thickness"] = 2;
        for (bool rotary : {false, true}) {
            Json run = Json::object();
            run["project"] = p;
            run["confirmed"] = true;
            if (rotary) {
                run["rotary"] = rotary_options();
                run["rotary_confirmed"] = true;
            }
            const kerfkite::http::Response response = route(studio, "POST", "/api/run", run);
            CHECK(response.status == 400);
            CHECK_TEXT(route_error(response), "Cut exceeds entered stock thickness");
            CHECK(studio.device().status().at("active") == Json(false));
        }
        studio.device().disconnect();
    }
    {
        // An xPRO laser job is checked against the firmware's own laser power scale.
        std::vector<std::string> writes;
        TableLink *opened = nullptr;
        MachineOptions machine = table({{"$I", {"[MSG:Using machine:CNC_xPRO_V5_XYAZ_NO]", "ok"}},
                                        {"$Spindle/Type", {"ok"}},
                                        {"$Laser/FullPower", {"$Laser/FullPower=255", "ok"}},
                                        {"$$", {"$32=1", "$30=20000", "$103=10", "$113=360", "$123=90", "$130=500", "$132=100", "ok"}},
                                        {"?", {"<Idle|WPos:0,0,0,0>"}}},
                                       &writes);
        const LinkFactory inner = machine.open;
        machine.open = [inner, &opened](const std::string &port) {
            std::unique_ptr<Link> link = inner(port);
            opened = static_cast<TableLink *>(link.get());
            return link;
        };
        Options server;
        server.enable_hardware = true;
        Studio studio(server, machine);
        CHECK(route(studio, "POST", "/api/connect", Json::parse(R"({"port": "fake", "protocol": "xpro-v5"})")).status == 200);
        CHECK(wait_until([&] { return studio.device().status().at("ready") == Json(true); }, 10));
        Json run = Json::object();
        run["project"] = rotary_project("xpro-v5", "laser");
        run["rotary"] = rotary_options("xpro-v5");
        run["confirmed"] = true;
        run["rotary_confirmed"] = true;
        CHECK_TEXT(route_error(route(studio, "POST", "/api/run", run)), "Spindle/Type");
        if (opened) opened->arrive("$Spindle/Type=LASER");
        CHECK(wait_until([&] { return text(studio.device().status().at("spindle_type")) == "LASER"; }, 5));
        CHECK_TEXT(route_error(route(studio, "POST", "/api/run", run)), "full-power limit");
        CHECK(studio.device().status().at("active") == Json(false));
        studio.device().disconnect();
    }
    for (const auto &pair : std::vector<std::pair<std::string, std::string>>{{"grblhal", "router"}, {"xpro-v5", "laser"}}) {
        // A rotary job streams to the simulator, with the usual confirmations.
        const std::string protocol = pair.first, mode = pair.second;
        Studio studio{Options(), MachineOptions()};
        CHECK(Json::parse(route(studio, "GET", "/api/session").body).at("rotary_stream") == Json(true));
        Json connect = Json::object();
        connect["port"] = "simulator";
        connect["protocol"] = protocol;
        CHECK(route(studio, "POST", "/api/connect", connect).status == 200);
        bool has_a = false;
        const Json status = Json::parse(route(studio, "GET", "/api/status").body);
        for (const Json &axis : status.at("rotary_axes")) has_a = has_a || text(axis) == "A";
        CHECK(has_a);
        Json p = rotary_project(protocol, mode);
        if (mode == "laser") p["path_powers"] = Json::parse("[[0, 75, 305]]");
        else p["path_depths"] = Json::parse("[[-0.5, -1, -2]]");
        const Json opts = rotary_options(protocol);
        Json payload = Json::object();
        payload["project"] = p;
        payload["rotary"] = opts;
        payload["confirmed"] = true;
        payload["rotary_confirmed"] = true;
        Json preview_request = Json::object();
        preview_request["project"] = p;
        preview_request["options"] = opts;
        const Json preview = Json::parse(route(studio, "POST", "/api/rotary", preview_request).body);
        CHECK(studio.device().status().at("active") == Json(false));
        const auto edited = [&](const char *key, Json value) {
            Json copy = payload;
            copy[key] = std::move(value);
            return route(studio, "POST", "/api/run", copy).status;
        };
        Json other = opts;
        other["postprocessor"] = "grbl";
        CHECK(edited("confirmed", Json(false)) == 400);
        CHECK(edited("rotary_confirmed", Json(false)) == 400);
        CHECK(edited("rotary", rotary_options("linuxcnc")) == 400);
        CHECK(edited("project", Json::parse(R"({"lines": ["G1 X1"]})")) == 400);
        CHECK(edited("rotary", other) == 400);
        Json arm = payload;
        arm["phone_arm_challenge"] = true;
        arm["rotary_confirmed"] = false;
        CHECK(route(studio, "POST", "/api/arm", arm).status == 400);
        arm["rotary_confirmed"] = true;
        const kerfkite::http::Response armed = route(studio, "POST", "/api/arm", arm);
        CHECK(armed.status == 200);
        Json disarm = Json::object();
        disarm["arm_id"] = Json::parse(armed.body).at("arm_id");
        route(studio, "POST", "/api/disarm", disarm);
        CHECK(route(studio, "POST", "/api/run", payload).status == 200);
        CHECK(wait_until([&] { return studio.device().status().at("active") == Json(false); }, 5));
        const Json state = studio.device().status();
        CHECK(text(state.at("state")) == "Idle");
        CHECK(state.at("progress") == Json(100));
        CHECK(state.at("position").is_object() && state.at("position").at("a") == Json(360));
        const Json log = state.at("log"), lines = preview.at("lines");
        bool tail = log.size() >= lines.size();
        for (size_t i = 0; tail && i < lines.size(); ++i) tail = log[log.size() - lines.size() + i] == lines[i];
        CHECK(tail);
        std::string joined;
        for (const Json &line : log) joined += text(line) + " ";
        CHECK_TEXT(joined, mode == "laser" ? "S305" : "Z-2.000000");
        studio.device().disconnect();
    }
}

}  // namespace

int main() {
    feed_hold();
    controller();
    marlin();
    describe();
    readback();
    simulated_probing();
    levelled_runs();
    rotary_readback();
    rotary_machines();
    for (const std::string &e : errors_seen) {
        std::fprintf(stderr, "refused in a script: %s\n", e.c_str());
        ++failures;  // a step the reference took without a refusal
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
