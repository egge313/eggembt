// Ways to carry the plug-in protocol (one JSON object per line).
//
//   StreamTransport    standard C++ only: any std::istream / std::ostream pair,
//                      typically std::cin and std::cout. Works everywhere, and
//                      lets a parent process drive the tool through pipes.
//   FdTransport        POSIX file descriptors (pipes, sockets), with a reader
//                      thread that can be stopped cleanly.
//   ProcessTransport   POSIX: starts an agent as a child process and talks to
//                      it over its standard input and output.
//   TcpListener and tcpConnect   POSIX: a TCP connection to or from an agent.
//
// The POSIX transports are only compiled where MBT_POSIX_IO is defined (Unix,
// Linux, macOS); on other systems use the stream transport.
//
// All transports deliver received lines in order, from a thread of their own,
// and report the end of the connection once through the close handler. sendLine
// may be called from any thread.
#pragma once

#include "engine/RemotePlugin.h"

#include <atomic>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define MBT_POSIX_IO 1
#endif

namespace pn {

// Splits a command line the way a shell would, without running one: whitespace
// separates words, single and double quotes group, a backslash escapes.
std::vector<std::string> splitCommandLine(const std::string &text);

class StreamTransport : public ILineTransport {
public:
    // The streams must stay valid for the life of the transport, or until they
    // reach end of file. A reader blocked in a stream that never ends (std::cin)
    // is abandoned when the transport is destroyed.
    StreamTransport(std::istream &in, std::ostream &out);
    ~StreamTransport() override;

    bool sendLine(const std::string &line) override;
    void setLineHandler(std::function<void(const std::string &)> handler) override;
    void setCloseHandler(std::function<void()> handler) override;

private:
    struct State;
    std::shared_ptr<State> state_;
    std::ostream &out_;
    std::mutex writeMutex_;
    std::thread reader_;
};

#ifdef MBT_POSIX_IO

class FdTransport : public ILineTransport {
public:
    // Reads from readFd and writes to writeFd (the same descriptor for a socket).
    // With ownFds the descriptors are closed on destruction.
    FdTransport(int readFd, int writeFd, bool ownFds);
    ~FdTransport() override;

    FdTransport(const FdTransport &) = delete;
    FdTransport &operator=(const FdTransport &) = delete;

    bool sendLine(const std::string &line) override;
    void setLineHandler(std::function<void(const std::string &)> handler) override;
    void setCloseHandler(std::function<void()> handler) override;

    // Stops reading and closes the write side; the peer sees end of file.
    void close();

private:
    struct State;
    static void readLoop(std::shared_ptr<State> state, int fd);

    std::shared_ptr<State> state_;
    int readFd_;
    int writeFd_;
    bool ownFds_;
    std::mutex writeMutex_;
    std::thread reader_;
};

class ProcessTransport : public ILineTransport {
public:
    // Starts `argv[0]` (searched in PATH) with its standard input and output
    // connected to this transport; its standard error is passed through.
    // Returns null, with `error` set, if it cannot be started.
    static std::unique_ptr<ProcessTransport> spawn(const std::vector<std::string> &argv, std::string *error);
    ~ProcessTransport() override;

    bool sendLine(const std::string &line) override { return io_->sendLine(line); }
    void setLineHandler(std::function<void(const std::string &)> handler) override { io_->setLineHandler(std::move(handler)); }
    void setCloseHandler(std::function<void()> handler) override { io_->setCloseHandler(std::move(handler)); }

private:
    ProcessTransport() = default;
    std::unique_ptr<FdTransport> io_;
    int pid_ = -1;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener &) = delete;
    TcpListener &operator=(const TcpListener &) = delete;

    // Listens on address:port (port 0: any free port, see port()).
    bool open(const std::string &address, int port, std::string *error);
    int port() const { return port_; }

    // Waits up to timeoutMs for one connection.
    std::unique_ptr<FdTransport> accept(int timeoutMs, std::string *error);

private:
    int fd_ = -1;
    int port_ = 0;
};

std::unique_ptr<FdTransport> tcpConnect(const std::string &host, int port, int timeoutMs, std::string *error);

#endif  // MBT_POSIX_IO

}  // namespace pn
