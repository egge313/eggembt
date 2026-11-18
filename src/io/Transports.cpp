#include "io/Transports.h"

#include <cstring>
#include <deque>
#include <istream>
#include <ostream>

#ifdef MBT_POSIX_IO
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pn {

constexpr std::size_t kMaxLineBytes = 16u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// splitCommandLine
// ---------------------------------------------------------------------------

std::vector<std::string> splitCommandLine(const std::string &text)
{
    std::vector<std::string> words;
    std::string current;
    bool inWord = false;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && i + 1 < text.size()
                       && (text[i + 1] == '"' || text[i + 1] == '\\')) {
                current += text[++i];
            } else {
                current += c;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
            inWord = true;
        } else if (c == '\\' && i + 1 < text.size()) {
            current += text[++i];
            inWord = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (inWord) {
                words.push_back(current);
                current.clear();
                inWord = false;
            }
        } else {
            current += c;
            inWord = true;
        }
    }
    if (inWord)
        words.push_back(current);
    return words;
}

// ---------------------------------------------------------------------------
// Shared line splitting
// ---------------------------------------------------------------------------

namespace {

// Appends `chunk` to `buffer` and calls `emit` for every complete, non-blank line.
template <class Emit>
void splitLines(std::string &buffer, const char *chunk, std::size_t size, Emit emit)
{
    buffer.append(chunk, size);
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = buffer.find('\n', start);
        if (nl == std::string::npos)
            break;
        std::string line = buffer.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        start = nl + 1;
        if (line.find_first_not_of(" \t") != std::string::npos)
            emit(line);
    }
    buffer.erase(0, start);
}

// Hands lines and the end of the connection to the handlers, in order. Lines
// that arrive before a handler is attached are kept and delivered, in order, the
// moment it is attached; so is an end of connection seen early.
struct Delivery {
    std::mutex order;  // held while a handler runs, so lines never overtake each other
    std::mutex mutex;  // guards the members below
    std::function<void(const std::string &)> handler;
    std::function<void()> closeHandler;
    std::deque<std::string> pending;
    bool closeSeen = false;
    bool closeDelivered = false;
    std::atomic<bool> stop{false};

    void line(const std::string &text)
    {
        std::lock_guard<std::mutex> o(order);
        if (stop)
            return;
        std::function<void(const std::string &)> h;
        {
            std::lock_guard<std::mutex> lock(mutex);
            h = handler;
            if (!h)
                pending.push_back(text);
        }
        if (h)
            h(text);
    }

    void closed()
    {
        std::lock_guard<std::mutex> o(order);
        if (stop)
            return;
        std::function<void()> c;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closeSeen = true;
            if (closeHandler && !closeDelivered) {
                closeDelivered = true;
                c = closeHandler;
            }
        }
        if (c)
            c();
    }

    void setHandler(std::function<void(const std::string &)> h)
    {
        std::lock_guard<std::mutex> o(order);
        {
            std::lock_guard<std::mutex> lock(mutex);
            handler = h;
        }
        if (!h)
            return;
        for (;;) {
            std::string next;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pending.empty())
                    break;
                next = std::move(pending.front());
                pending.pop_front();
            }
            h(next);
        }
    }

    void setCloseHandler(std::function<void()> c)
    {
        std::lock_guard<std::mutex> o(order);
        bool now = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closeHandler = c;
            if (c && closeSeen && !closeDelivered) {
                closeDelivered = true;
                now = true;
            }
        }
        if (now)
            c();
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// StreamTransport (standard C++ only)
// ---------------------------------------------------------------------------

struct StreamTransport::State : Delivery {
    std::atomic<bool> done{false};
};

StreamTransport::StreamTransport(std::istream &in, std::ostream &out)
    : state_(std::make_shared<State>()), out_(out)
{
    std::shared_ptr<State> state = state_;
    std::istream *input = &in;
    reader_ = std::thread([state, input] {
        std::string line;
        while (!state->stop && std::getline(*input, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos)
                continue;
            state->line(line);
        }
        state->closed();
        state->done = true;
    });
}

StreamTransport::~StreamTransport()
{
    state_->stop = true;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->handler = nullptr;
        state_->closeHandler = nullptr;
    }
    // A reader blocked in a stream that never ends cannot be woken: abandon it.
    // It only touches shared state from here on, and the stream, which the
    // caller keeps alive.
    if (reader_.joinable()) {
        if (state_->done)
            reader_.join();
        else
            reader_.detach();
    }
}

bool StreamTransport::sendLine(const std::string &line)
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    out_ << line << '\n' << std::flush;
    return out_.good();
}

void StreamTransport::setLineHandler(std::function<void(const std::string &)> handler)
{
    state_->setHandler(std::move(handler));
}

void StreamTransport::setCloseHandler(std::function<void()> handler)
{
    state_->setCloseHandler(std::move(handler));
}

#ifdef MBT_POSIX_IO

// ---------------------------------------------------------------------------
// FdTransport
// ---------------------------------------------------------------------------

struct FdTransport::State : Delivery {};

FdTransport::FdTransport(int readFd, int writeFd, bool ownFds)
    : state_(std::make_shared<State>()), readFd_(readFd), writeFd_(writeFd), ownFds_(ownFds)
{
    // Writing to a connection the peer has closed must fail with an error, not kill the process.
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
    reader_ = std::thread(&FdTransport::readLoop, state_, readFd_);
}

FdTransport::~FdTransport()
{
    close();
}

void FdTransport::close()
{
    state_->stop = true;
    if (reader_.joinable())
        reader_.join();
    if (ownFds_) {
        if (readFd_ >= 0)
            ::close(readFd_);
        if (writeFd_ >= 0 && writeFd_ != readFd_)
            ::close(writeFd_);
        ownFds_ = false;
    }
    readFd_ = writeFd_ = -1;
}

void FdTransport::setLineHandler(std::function<void(const std::string &)> handler)
{
    state_->setHandler(std::move(handler));
}

void FdTransport::setCloseHandler(std::function<void()> handler)
{
    state_->setCloseHandler(std::move(handler));
}

bool FdTransport::sendLine(const std::string &line)
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (writeFd_ < 0)
        return false;
    const std::string data = line + "\n";
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::write(writeFd_, data.data() + sent, data.size() - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{writeFd_, POLLOUT, 0};
            ::poll(&p, 1, 100);
        } else {
            return false;
        }
    }
    return true;
}

void FdTransport::readLoop(std::shared_ptr<State> state, int fd)
{
    std::string buffer;
    char chunk[4096];
    while (!state->stop) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 50);  // wakes up regularly to notice a stop request
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            continue;
        const ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n > 0) {
            splitLines(buffer, chunk, static_cast<std::size_t>(n),
                       [&](const std::string &line) { state->line(line); });
            if (buffer.size() > kMaxLineBytes)
                break;  // a line without end: not our protocol
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        break;  // end of file or an error
    }
    state->closed();
}

// ---------------------------------------------------------------------------
// ProcessTransport
// ---------------------------------------------------------------------------

std::unique_ptr<ProcessTransport> ProcessTransport::spawn(const std::vector<std::string> &argv, std::string *error)
{
    if (argv.empty()) {
        if (error)
            *error = "empty agent command";
        return nullptr;
    }
    int toChild[2], fromChild[2], execErr[2];
    if (::pipe(toChild) != 0 || ::pipe(fromChild) != 0 || ::pipe(execErr) != 0) {
        if (error)
            *error = std::string("cannot create pipes: ") + std::strerror(errno);
        return nullptr;
    }
    ::fcntl(execErr[1], F_SETFD, FD_CLOEXEC);

    std::vector<char *> args;
    for (const std::string &a : argv)
        args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error)
            *error = std::string("cannot start the agent: ") + std::strerror(errno);
        for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1], execErr[0], execErr[1]})
            ::close(fd);
        return nullptr;
    }
    if (pid == 0) {  // child
        ::dup2(toChild[0], 0);
        ::dup2(fromChild[1], 1);
        for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1], execErr[0]})
            ::close(fd);
        ::execvp(args[0], args.data());
        const int e = errno;
        const ssize_t ignored = ::write(execErr[1], &e, sizeof e);
        (void)ignored;
        ::_exit(127);
    }

    // parent
    ::close(toChild[0]);
    ::close(fromChild[1]);
    ::close(execErr[1]);
    int childErrno = 0;
    ssize_t got;
    do {
        got = ::read(execErr[0], &childErrno, sizeof childErrno);
    } while (got < 0 && errno == EINTR);
    ::close(execErr[0]);
    if (got > 0) {  // exec failed
        ::close(toChild[1]);
        ::close(fromChild[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (error)
            *error = "cannot run \"" + argv[0] + "\": " + std::strerror(childErrno);
        return nullptr;
    }

    std::unique_ptr<ProcessTransport> t(new ProcessTransport());
    t->pid_ = pid;
    t->io_ = std::make_unique<FdTransport>(fromChild[0], toChild[1], true);
    return t;
}

ProcessTransport::~ProcessTransport()
{
    io_.reset();  // closes the pipes: the agent sees end of file on its standard input
    if (pid_ <= 0)
        return;
    int status = 0;
    for (int i = 0; i < 100; ++i) {  // give it a second to leave on its own
        if (::waitpid(pid_, &status, WNOHANG) == pid_)
            return;
        ::usleep(10000);
    }
    ::kill(pid_, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(pid_, &status, WNOHANG) == pid_)
            return;
        ::usleep(10000);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------

namespace {

bool setNonBlocking(int fd, bool on)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}

void setError(std::string *error, const std::string &text)
{
    if (error)
        *error = text;
}

}  // namespace

TcpListener::~TcpListener()
{
    if (fd_ >= 0)
        ::close(fd_);
}

bool TcpListener::open(const std::string &address, int port, std::string *error)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo *res = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        setError(error, "cannot resolve " + address + ": " + ::gai_strerror(rc));
        return false;
    }
    std::string lastError = "no usable address";
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastError = std::strerror(errno);
            continue;
        }
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 1) == 0) {
            sockaddr_storage local;
            socklen_t len = sizeof local;
            ::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &len);
            port_ = local.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6 *>(&local)->sin6_port)
                                                : ntohs(reinterpret_cast<sockaddr_in *>(&local)->sin_port);
            fd_ = fd;
            ::freeaddrinfo(res);
            return true;
        }
        lastError = std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(res);
    setError(error, "cannot listen on " + address + ":" + service + ": " + lastError);
    return false;
}

std::unique_ptr<FdTransport> TcpListener::accept(int timeoutMs, std::string *error)
{
    if (fd_ < 0) {
        setError(error, "not listening");
        return nullptr;
    }
    pollfd p{fd_, POLLIN, 0};
    int r;
    do {
        r = ::poll(&p, 1, timeoutMs);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
        setError(error, r == 0 ? "no agent connected in time" : std::string("accept failed: ") + std::strerror(errno));
        return nullptr;
    }
    const int fd = ::accept(fd_, nullptr, nullptr);
    if (fd < 0) {
        setError(error, std::string("accept failed: ") + std::strerror(errno));
        return nullptr;
    }
    return std::make_unique<FdTransport>(fd, fd, true);
}

std::unique_ptr<FdTransport> tcpConnect(const std::string &host, int port, int timeoutMs, std::string *error)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        setError(error, "cannot resolve " + host + ": " + ::gai_strerror(rc));
        return nullptr;
    }
    std::string lastError = "no usable address";
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastError = std::strerror(errno);
            continue;
        }
        setNonBlocking(fd, true);
        int c = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (c != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, timeoutMs) == 1) {
                int soError = 0;
                socklen_t len = sizeof soError;
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
                c = soError == 0 ? 0 : -1;
                errno = soError;
            } else {
                c = -1;
                errno = ETIMEDOUT;
            }
        }
        if (c == 0) {
            setNonBlocking(fd, false);
            ::freeaddrinfo(res);
            return std::make_unique<FdTransport>(fd, fd, true);
        }
        lastError = std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(res);
    setError(error, "cannot connect to " + host + ":" + service + ": " + lastError);
    return nullptr;
}

#endif  // MBT_POSIX_IO

}  // namespace pn
