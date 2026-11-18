// JSON-lines run log: one header line, then one line per finished run.
//
//   {"type":"header","format":"mbt-runlog","version":1,"graph":"<fingerprint>"}
//   {"type":"run","initial":"start","steps":["open_cmd",...],"outcome":"completed", ...}
//   {"type":"run","initial":"start","steps":[],"events":[{"kind":"stimulated","label":"door.open","t":12},...],...}
//
// "steps" holds the transition ids of a path run; "events" holds the live
// events of an event run (see engine/RunLog.h).
//
// Runs are appended and flushed to the operating system as each run ends, so a
// crash of the SUT (or of the tester) loses at most the run in progress. The
// reader tolerates a truncated last line, which is what an interrupted append
// leaves, and the writer cuts such a fragment off before appending. The header
// ties the file to a model (see graphFingerprint); the writer refuses to append
// to a log of a different model.
#pragma once

#include "engine/RunLog.h"

#include <fstream>
#include <string>
#include <vector>

namespace pn {

struct RunLogContents {
    std::string fingerprint;
    std::vector<PersistedRun> runs;
    std::vector<std::string> warnings;  // e.g. "line 7: ignored truncated last line"
};

struct RunLogReadResult {
    bool ok = false;
    std::string error;
    RunLogContents contents;
};

RunLogReadResult readRunLog(const std::string &path);

class RunLogWriter {
public:
    // Creates the file with a header, or validates the header of an existing
    // file and appends to it.
    bool open(const std::string &path, const std::string &fingerprint, std::string *error);
    bool append(const PersistedRun &run, std::string *error);  // one line, flushed
    void close();
    bool isOpen() const { return file_.is_open(); }

private:
    std::ofstream file_;
};

}  // namespace pn
