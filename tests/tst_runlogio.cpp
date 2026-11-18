#include "MiniTest.h"
#include "TempDir.h"
#include "io/RunLogIO.h"

using namespace pn;

namespace {

PersistedRun sampleRun(const char *note, RunOutcome outcome)
{
    PersistedRun r;
    r.initialMarking = "start";
    r.transitions = {"open_cmd", "opened", "close_cmd", "jam"};
    r.outcome = outcome;
    r.detail = "jammed";
    r.note = note;
    if (outcome == RunOutcome::ErrorReached) {
        DumpRecord d;
        d.stepIndex = 4;
        d.marking = "jammed";
        d.mode = DumpMode::Snapshot;
        d.files = {"/tmp/core.1234", "/tmp/stack.txt"};
        d.stackText = "#0 jam()\n#1 main() \"quoted\" \xc3\xa4";  // newline, quotes, UTF-8
        r.dumps.push_back(d);
    }
    return r;
}

}  // namespace

struct TstRunLogIO {
    void roundTrip()
    {
        TempDir dir;
        const std::string path = dir.filePath("runs.jsonl");

        RunLogWriter w;
        std::string err;
        T_VERIFY2(w.open(path, "0123456789abcdef", &err), minitest::cstr(err));
        T_VERIFY2(w.append(sampleRun("one", RunOutcome::Completed), &err), minitest::cstr(err));
        T_VERIFY2(w.append(sampleRun("two", RunOutcome::ErrorReached), &err), minitest::cstr(err));
        w.close();

        const RunLogReadResult r = readRunLog(path);
        T_VERIFY2(r.ok, minitest::cstr(r.error));
        T_VERIFY(r.contents.warnings.empty());
        T_COMPARE(r.contents.fingerprint, std::string("0123456789abcdef"));
        T_COMPARE(r.contents.runs.size(), std::size_t(2));

        const PersistedRun &a = r.contents.runs[0];
        T_COMPARE(a.initialMarking, std::string("start"));
        T_COMPARE(a.transitions.size(), std::size_t(4));
        T_COMPARE(a.transitions[3], std::string("jam"));
        T_COMPARE(a.outcome, RunOutcome::Completed);
        T_COMPARE(a.note, std::string("one"));
        T_VERIFY(a.dumps.empty());

        const PersistedRun &b = r.contents.runs[1];
        T_COMPARE(b.outcome, RunOutcome::ErrorReached);
        T_COMPARE(b.detail, std::string("jammed"));
        T_COMPARE(b.dumps.size(), std::size_t(1));
        T_COMPARE(b.dumps[0].stepIndex, std::uint32_t(4));
        T_COMPARE(b.dumps[0].marking, std::string("jammed"));
        T_COMPARE(b.dumps[0].mode, DumpMode::Snapshot);
        T_COMPARE(b.dumps[0].files.size(), std::size_t(2));
        T_COMPARE(b.dumps[0].files[0], std::string("/tmp/core.1234"));
        T_COMPARE(b.dumps[0].stackText, sampleRun("two", RunOutcome::ErrorReached).dumps[0].stackText);
    }

    // GLM-5.3-Flash: the hook records travel with the run (doku section 18);
    // the commands themselves are not part of the log.
    void hookRunsRoundTrip()
    {
        TempDir dir;
        const std::string path = dir.filePath("hooks.jsonl");

        PersistedRun run;
        run.initialMarking = "start";
        run.transitions = {"collect"};
        run.outcome = RunOutcome::Completed;
        HookRecord ok;
        ok.transition = "collect";
        ok.label = "press";
        ok.status = "ok";
        ok.exit = 0;
        ok.output = "collected 42\nline two";
        run.hooks.push_back(ok);
        HookRecord bad;
        bad.transition = "collect";
        bad.label = "press";
        bad.status = "timeout";
        bad.exit = kHookNoExit;
        run.hooks.push_back(bad);
        HookRecord launched;
        launched.transition = "verify";
        launched.label = "audit";
        launched.async = true;
        launched.status = "launched";
        run.hooks.push_back(launched);

        RunLogWriter w;
        std::string err;
        T_VERIFY2(w.open(path, "hooks", &err), minitest::cstr(err));
        T_VERIFY2(w.append(run, &err), minitest::cstr(err));
        w.close();

        const RunLogReadResult r = readRunLog(path);
        T_VERIFY2(r.ok, minitest::cstr(r.error));
        T_COMPARE(r.contents.runs.size(), std::size_t(1));
        T_VERIFY(r.contents.runs[0].hooks.size() == 3);
        if (r.contents.runs[0].hooks.size() == 3) {
            const HookRecord &a = r.contents.runs[0].hooks[0];
            T_COMPARE(a.transition, std::string("collect"));
            T_COMPARE(a.label, std::string("press"));
            T_VERIFY(!a.async);
            T_COMPARE(a.status, std::string("ok"));
            T_COMPARE(a.exit, 0);
            T_COMPARE(a.output, std::string("collected 42\nline two"));
            const HookRecord &b = r.contents.runs[0].hooks[1];
            T_COMPARE(b.status, std::string("timeout"));
            T_COMPARE(b.exit, kHookNoExit);
            T_VERIFY(b.output.empty());
            const HookRecord &c = r.contents.runs[0].hooks[2];
            T_VERIFY(c.async);
            T_COMPARE(c.status, std::string("launched"));
            T_COMPARE(c.transition, std::string("verify"));
        }
    }

    void eventRunsRoundTrip()
    {
        TempDir dir;
        const std::string path = dir.filePath("events.jsonl");

        PersistedRun run;
        run.initialMarking = "start";
        run.events = {SutEvent{EventKind::Stimulated, "door.open", 0}, SutEvent{EventKind::Observed, "door.opened", 1250},
                      SutEvent{EventKind::Quiescent, {}, 9000}};
        run.outcome = RunOutcome::Deviation;
        run.detail = "missing output";

        RunLogWriter w;
        std::string err;
        T_VERIFY2(w.open(path, "ffff", &err), minitest::cstr(err));
        T_VERIFY2(w.append(run, &err), minitest::cstr(err));
        w.close();

        const RunLogReadResult r = readRunLog(path);
        T_VERIFY2(r.ok, minitest::cstr(r.error));
        T_COMPARE(r.contents.runs.size(), std::size_t(1));
        const PersistedRun &back = r.contents.runs[0];
        T_VERIFY(back.transitions.empty());
        T_COMPARE(back.events.size(), std::size_t(3));
        T_COMPARE(back.events[0].kind, EventKind::Stimulated);
        T_COMPARE(back.events[0].label, std::string("door.open"));
        T_COMPARE(back.events[0].timestampMs, std::int64_t(0));
        T_COMPARE(back.events[1].timestampMs, std::int64_t(1250));
        T_COMPARE(back.events[2].timestampMs, std::int64_t(9000));
        T_COMPARE(back.events[2].kind, EventKind::Quiescent);
        T_VERIFY(back.events[2].label.empty());
        T_COMPARE(back.outcome, RunOutcome::Deviation);
    }

    void appendsAcrossSessions()
    {
        TempDir dir;
        const std::string path = dir.filePath("runs.jsonl");
        std::string err;
        {
            RunLogWriter w;
            T_VERIFY2(w.open(path, "aaaa", &err), minitest::cstr(err));
            T_VERIFY(w.append(sampleRun("first session", RunOutcome::Completed), &err));
        }
        {
            RunLogWriter w;
            T_VERIFY2(w.open(path, "aaaa", &err), minitest::cstr(err));
            T_VERIFY(w.append(sampleRun("second session", RunOutcome::Completed), &err));
        }
        const RunLogReadResult r = readRunLog(path);
        T_VERIFY2(r.ok, minitest::cstr(r.error));
        T_COMPARE(r.contents.runs.size(), std::size_t(2));
        T_COMPARE(r.contents.runs[1].note, std::string("second session"));
    }

    void refusesLogOfAnotherModel()
    {
        TempDir dir;
        const std::string path = dir.filePath("runs.jsonl");
        std::string err;
        RunLogWriter first;
        T_VERIFY(first.open(path, "aaaa", &err));
        first.close();

        RunLogWriter other;
        T_VERIFY(!other.open(path, "bbbb", &err));
        T_VERIFY2(minitest::contains(err, "different model"), minitest::cstr(err));
        T_VERIFY(!other.isOpen());
    }

    void toleratesTruncatedLastLine()
    {
        TempDir dir;
        const std::string path = dir.filePath("runs.jsonl");
        std::string err;
        {
            RunLogWriter w;
            T_VERIFY(w.open(path, "aaaa", &err));
            T_VERIFY(w.append(sampleRun("kept", RunOutcome::Completed), &err));
        }
        dir.append("runs.jsonl", "{\"type\":\"run\",\"init");  // an interrupted append, no newline

        const RunLogReadResult r = readRunLog(path);
        T_VERIFY2(r.ok, minitest::cstr(r.error));
        T_COMPARE(r.contents.runs.size(), std::size_t(1));
        T_COMPARE(r.contents.warnings.size(), std::size_t(1));
        T_VERIFY(minitest::contains(r.contents.warnings[0], "truncated"));

        // A writer drops the unfinished fragment and resumes cleanly.
        RunLogWriter w;
        T_VERIFY2(w.open(path, "aaaa", &err), minitest::cstr(err));
        T_VERIFY(w.append(sampleRun("after", RunOutcome::Completed), &err));
        w.close();
        const RunLogReadResult again = readRunLog(path);
        T_VERIFY2(again.ok, minitest::cstr(again.error));
        T_VERIFY(again.contents.warnings.empty());
        T_COMPARE(again.contents.runs.size(), std::size_t(2));
        T_COMPARE(again.contents.runs[0].note, std::string("kept"));
        T_COMPARE(again.contents.runs[1].note, std::string("after"));
    }

    void damagedLineInTheMiddleIsAnError()
    {
        TempDir dir;
        dir.append("runs.jsonl",
                   "{\"type\":\"header\",\"format\":\"mbt-runlog\",\"version\":1,\"graph\":\"x\"}\n"
                   "{\"type\":\"run\",\"init\n"
                   "{\"type\":\"run\",\"initial\":\"s\",\"steps\":[],\"outcome\":\"completed\"}\n");
        const RunLogReadResult r = readRunLog(dir.filePath("runs.jsonl"));
        T_VERIFY(!r.ok);
        T_VERIFY(minitest::contains(r.error, "line 2"));
    }

    void rejectsGarbageAndUnknownOutcome()
    {
        TempDir dir;
        dir.append("garbage.jsonl", "hello\n");
        T_VERIFY(!readRunLog(dir.filePath("garbage.jsonl")).ok);

        dir.append("bad.jsonl",
                   "{\"type\":\"header\",\"format\":\"mbt-runlog\",\"version\":1,\"graph\":\"x\"}\n"
                   "{\"type\":\"run\",\"initial\":\"s\",\"steps\":[],\"outcome\":\"open\"}\n"
                   "{\"type\":\"run\",\"initial\":\"s\",\"steps\":[],\"outcome\":\"completed\"}\n");
        const RunLogReadResult r = readRunLog(dir.filePath("bad.jsonl"));
        T_VERIFY(!r.ok);
        T_VERIFY(minitest::contains(r.error, "line 2"));

        T_VERIFY(!readRunLog(dir.filePath("missing.jsonl")).ok);
    }
};

int main()
{
    T_RUN(TstRunLogIO, roundTrip);
    T_RUN(TstRunLogIO, hookRunsRoundTrip);  // GLM-5.3-Flash
    T_RUN(TstRunLogIO, eventRunsRoundTrip);
    T_RUN(TstRunLogIO, appendsAcrossSessions);
    T_RUN(TstRunLogIO, refusesLogOfAnotherModel);
    T_RUN(TstRunLogIO, toleratesTruncatedLastLine);
    T_RUN(TstRunLogIO, damagedLineInTheMiddleIsAnError);
    T_RUN(TstRunLogIO, rejectsGarbageAndUnknownOutcome);
    return T_SUMMARY();
}
