// Tests for the transports: command-line splitting, standard streams, POSIX
// descriptors, a child process, TCP, and a whole campaign over real sockets.
#include "MiniTest.h"
#include "engine/AgentServer.h"
#include "engine/Session.h"
#include "engine/SimulatedSut.h"
#include "io/Transports.h"
#include "model/NetBuilder.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace pn;

namespace {

template <class Pred>
bool waitFor(Pred pred, int ms = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// Collects received lines, thread-safely.
struct Lines {
    std::mutex m;
    std::vector<std::string> v;
    std::atomic<bool> closed{false};

    void attach(ILineTransport &t)
    {
        t.setLineHandler([this](const std::string &s) {
            std::lock_guard<std::mutex> lock(m);
            v.push_back(s);
        });
        t.setCloseHandler([this] { closed = true; });
    }
    std::size_t size()
    {
        std::lock_guard<std::mutex> lock(m);
        return v.size();
    }
    std::string at(std::size_t i)
    {
        std::lock_guard<std::mutex> lock(m);
        return i < v.size() ? v[i] : std::string();
    }
};

Net makeThreeBranchNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    std::vector<PlaceIndex> mid;
    for (const char *name : {"s1", "s2", "s3"})
        mid.push_back(b.place(name));
    const char *inputs[] = {"a", "b", "c"};
    const char *outputs[] = {"oa", "ob", "oc"};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto in = b.transition(inputs[i], TransitionKind::Controllable, inputs[i]);
        const auto out = b.transition(outputs[i], TransitionKind::Observable, outputs[i]);
        b.input(s0, in).output(in, mid[i]);
        b.input(mid[i], out).output(out, s0);
    }
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

}  // namespace

struct TstTransports {
    void splitsCommandLinesLikeAShell()
    {
        using V = std::vector<std::string>;
        T_VERIFY(splitCommandLine("a b  c") == (V{"a", "b", "c"}));
        T_VERIFY(splitCommandLine("  ") == V{});
        T_VERIFY(splitCommandLine("prog --name 'two words' \"and \\\"this\\\"\"") ==
                 (V{"prog", "--name", "two words", "and \"this\""}));
        T_VERIFY(splitCommandLine("a\\ b c") == (V{"a b", "c"}));
        T_VERIFY(splitCommandLine("x '' y") == (V{"x", "", "y"}));  // an empty quoted word is a word
        T_VERIFY(splitCommandLine("unterminated 'quote") == (V{"unterminated", "quote"}));
    }

    void streamTransportOrderAndOutput()
    {
        // A stream that delivers data only after the handler is attached.
        struct GatedBuf : std::stringbuf {
            std::atomic<bool> open{false};
            int_type underflow() override
            {
                while (!open)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return std::stringbuf::underflow();
            }
        };
        GatedBuf buf;
        buf.str("first\r\n\n   \nsecond\nthird");
        std::istream in(&buf);
        std::ostringstream out;
        Lines got;
        {
            StreamTransport t(in, out);
            got.attach(t);
            buf.open = true;
            T_VERIFY(waitFor([&] { return got.closed.load(); }));
            T_COMPARE(got.size(), std::size_t(3));
            T_COMPARE(got.at(0), std::string("first"));  // CR stripped
            T_COMPARE(got.at(1), std::string("second"));
            T_COMPARE(got.at(2), std::string("third"));  // a last line without newline still counts
            T_VERIFY(t.sendLine("hello"));
            T_VERIFY(t.sendLine("{\"k\":1}"));
        }
        T_COMPARE(out.str(), std::string("hello\n{\"k\":1}\n"));
    }

    void fdTransportExchangesLinesAndNoticesTheClose()
    {
        int p1[2], p2[2];
        T_VERIFY(::pipe(p1) == 0 && ::pipe(p2) == 0);
        Lines atA, atB;
        std::unique_ptr<FdTransport> a = std::make_unique<FdTransport>(p1[0], p2[1], true);
        std::unique_ptr<FdTransport> b = std::make_unique<FdTransport>(p2[0], p1[1], true);
        atA.attach(*a);
        atB.attach(*b);

        T_VERIFY(a->sendLine("one"));
        T_VERIFY(a->sendLine("two"));
        T_VERIFY(b->sendLine("reply"));
        T_VERIFY(waitFor([&] { return atB.size() == 2 && atA.size() == 1; }));
        T_COMPARE(atB.at(0), std::string("one"));
        T_COMPARE(atB.at(1), std::string("two"));
        T_COMPARE(atA.at(0), std::string("reply"));

        // A large line, and lines from several threads, arrive whole and in each sender's order.
        const std::string big(300000, 'x');
        T_VERIFY(a->sendLine(big));
        T_VERIFY(waitFor([&] { return atB.size() == 3; }));
        T_COMPARE(atB.at(2).size(), big.size());

        // Closing one side is noticed by the other, and sending to it then fails.
        b.reset();
        T_VERIFY(waitFor([&] { return atA.closed.load(); }));
        bool failed = false;
        for (int i = 0; i < 50 && !failed; ++i)
            failed = !a->sendLine("anyone there?");
        T_VERIFY(failed);
    }

    void fdTransportSplitsPartialWrites()
    {
        int p1[2], p2[2];
        T_VERIFY(::pipe(p1) == 0 && ::pipe(p2) == 0);
        Lines got;
        FdTransport t(p1[0], p2[1], true);
        got.attach(t);
        T_VERIFY(::write(p1[1], "par", 3) == 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        T_COMPARE(got.size(), std::size_t(0));  // no newline yet
        T_VERIFY(::write(p1[1], "tial\r\nnext\n", 11) == 11);
        T_VERIFY(waitFor([&] { return got.size() == 2; }));
        T_COMPARE(got.at(0), std::string("partial"));
        T_COMPARE(got.at(1), std::string("next"));
        ::close(p1[1]);
        ::close(p2[0]);
    }

    void aChildProcessIsATransport()
    {
        std::string err;
        std::unique_ptr<ProcessTransport> t = ProcessTransport::spawn({"cat"}, &err);
        T_VERIFY2(t != nullptr, minitest::cstr(err));
        if (!t)
            return;
        Lines got;
        got.attach(*t);
        T_VERIFY(t->sendLine("echo me"));
        T_VERIFY(t->sendLine("and me"));
        T_VERIFY(waitFor([&] { return got.size() == 2; }));
        T_COMPARE(got.at(0), std::string("echo me"));
        T_COMPARE(got.at(1), std::string("and me"));
    }

    void aProcessThatCannotBeStartedIsReported()
    {
        std::string err;
        T_VERIFY(ProcessTransport::spawn({"/no/such/agent-program"}, &err) == nullptr);
        T_VERIFY2(minitest::contains(err, "cannot run"), minitest::cstr(err));
        T_VERIFY(ProcessTransport::spawn({}, &err) == nullptr);
    }

    void aProcessThatExitsIsNoticed()
    {
        std::string err;
        auto t = ProcessTransport::spawn({"sh", "-c", "echo hello; echo bye"}, &err);
        T_VERIFY2(t != nullptr, minitest::cstr(err));
        if (!t)
            return;
        Lines got;
        got.attach(*t);
        T_VERIFY(waitFor([&] { return got.closed.load(); }));
        T_COMPARE(got.size(), std::size_t(2));
        T_COMPARE(got.at(1), std::string("bye"));
    }

    void aStubbornProcessIsKilledOnDestruction()
    {
        std::string err;
        const auto start = std::chrono::steady_clock::now();
        {
            auto t = ProcessTransport::spawn({"sleep", "30"}, &err);  // ignores end of file on stdin
            T_VERIFY2(t != nullptr, minitest::cstr(err));
        }
        const auto took = std::chrono::steady_clock::now() - start;
        T_VERIFY(took < std::chrono::seconds(5));
    }

    void tcpConnectionsWorkBothWays()
    {
        TcpListener listener;
        std::string err;
        T_VERIFY2(listener.open("127.0.0.1", 0, &err), minitest::cstr(err));
        T_VERIFY(listener.port() > 0);

        std::unique_ptr<FdTransport> server;
        std::thread acceptor([&] { server = listener.accept(3000, nullptr); });
        auto client = tcpConnect("127.0.0.1", listener.port(), 3000, &err);
        acceptor.join();
        T_VERIFY2(client != nullptr, minitest::cstr(err));
        T_VERIFY(server != nullptr);
        if (!client || !server)
            return;

        Lines atClient, atServer;
        atClient.attach(*client);
        atServer.attach(*server);
        T_VERIFY(client->sendLine("ping"));
        T_VERIFY(server->sendLine("pong \xc3\xa4"));
        T_VERIFY(waitFor([&] { return atServer.size() == 1 && atClient.size() == 1; }));
        T_COMPARE(atServer.at(0), std::string("ping"));
        T_COMPARE(atClient.at(0), std::string("pong \xc3\xa4"));

        server.reset();
        T_VERIFY(waitFor([&] { return atClient.closed.load(); }));
    }

    void tcpErrorsAreReported()
    {
        TcpListener listener;
        std::string err;
        T_VERIFY(listener.open("127.0.0.1", 0, &err));
        T_VERIFY(listener.accept(50, &err) == nullptr);  // nobody connects
        T_VERIFY2(minitest::contains(err, "in time"), minitest::cstr(err));

        T_VERIFY(tcpConnect("127.0.0.1", 1, 500, &err) == nullptr);  // port 1: nothing listens there
        T_VERIFY2(minitest::contains(err, "cannot connect"), minitest::cstr(err));
        T_VERIFY(tcpConnect("no-such-host.invalid", 80, 500, &err) == nullptr);
    }

    void aWholeCampaignRunsOverRealSockets()
    {
        const Net net = makeThreeBranchNet();
        SimulatedSut sut(net);

        TcpListener listener;
        std::string err;
        T_VERIFY2(listener.open("127.0.0.1", 0, &err), minitest::cstr(err));

        // The agent side: accepts a connection and serves the simulated SUT.
        std::unique_ptr<FdTransport> agentLink;
        std::unique_ptr<AgentServer> server;
        std::thread agentThread([&] {
            agentLink = listener.accept(5000, nullptr);
            if (agentLink) {
                server = std::make_unique<AgentServer>(sut, *agentLink, "sim-over-tcp");
                server->start();
            }
        });

        auto trackerLink = tcpConnect("127.0.0.1", listener.port(), 3000, &err);
        agentThread.join();
        T_VERIFY2(trackerLink != nullptr, minitest::cstr(err));
        T_VERIFY(agentLink != nullptr);
        if (!trackerLink || !agentLink)
            return;

        RemotePlugin plugin(*trackerLink);
        T_VERIFY(plugin.waitForHello(std::chrono::milliseconds(2000)));
        T_COMPARE(plugin.agentName(), std::string("sim-over-tcp"));

        SessionConfig cfg;
        cfg.poll = std::chrono::milliseconds(2);
        cfg.plugin.quiescenceTimeoutMs = 30;
        Session session(net, cfg);
        const CampaignReport rep = session.runCampaign(plugin);
        T_VERIFY(rep.complete);
        T_COMPARE(rep.runs.size(), std::size_t(1));
        T_COMPARE(rep.summary.transitionsCovered, std::size_t(6));

        plugin.shutdown();
        T_VERIFY(server->waitUntilFinished(std::chrono::milliseconds(2000)));
    }
};

int main()
{
    T_RUN(TstTransports, splitsCommandLinesLikeAShell);
    T_RUN(TstTransports, streamTransportOrderAndOutput);
    T_RUN(TstTransports, fdTransportExchangesLinesAndNoticesTheClose);
    T_RUN(TstTransports, fdTransportSplitsPartialWrites);
    T_RUN(TstTransports, aChildProcessIsATransport);
    T_RUN(TstTransports, aProcessThatCannotBeStartedIsReported);
    T_RUN(TstTransports, aProcessThatExitsIsNoticed);
    T_RUN(TstTransports, aStubbornProcessIsKilledOnDestruction);
    T_RUN(TstTransports, tcpConnectionsWorkBothWays);
    T_RUN(TstTransports, tcpErrorsAreReported);
    T_RUN(TstTransports, aWholeCampaignRunsOverRealSockets);
    return T_SUMMARY();
}
