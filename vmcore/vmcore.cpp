// =============================================================================
// vmcore.cpp -- the VM runtime translation unit.
//
// Drives bytecode through the register-resident while{switch} interpreter
// (run_switch, Interpreter.h) and exposes execute() (declared in Execute.h).
// Every other project links this as a static library and reaches the runtime
// solely through execute().
//
// The threaded tail-call handlers and their dispatch_table are gone entirely:
// they were retired to a separate benchmark project (vm_bench) when vmcore moved
// to while{switch}, and that project was removed earlier. run_switch owns the fault
// frame that translates guard-page / illegal-instruction / divide-by-zero faults into
// std::runtime_error -- an SEH __try/__except on Windows, a sigsetjmp landing with a
// SIGSEGV/SIGBUS handler elsewhere (FaultSignals.h). See "Dispatch" in docs/VirtualMachine.md.
// =============================================================================

#include <stdexcept>
#include <vector>
#include <string>
#include <iostream>   // std::cout -- the default PRINT/PRINTLN sink
#include <fstream>    // readFile / writeFile natives
#include <iterator>   // std::istreambuf_iterator (readFile)
#include <chrono>     // millisTime native (wall-clock ms since epoch)
#include <cmath>      // std::math natives (sqrt/pow/sin/... + isnan/isinf)
#include <cstdlib>    // std::getenv / std::free
#include <filesystem> // fileExists / deleteFile natives
#include <thread>     // reader threads that drain a child process's stdout/stderr (rawRun); fork-join tasks
#include <memory>     // std::shared_ptr -- an isolate record outlives whoever still looks at it
#include <sstream>    // a task's own output / input streams
#include <mutex>      // the world registry, the mailboxes, the shared output line lock
#include <condition_variable>   // a blocking receive
#include <deque>      // a mailbox's queue
#include <unordered_map>        // the world registry: isolate id -> record
#include <atomic>     // World::actors_started, read by the root's output buffer
#include <optional>   // the world a root execute() owns
#ifdef _WIN32
// Winsock MUST precede <windows.h> to avoid winsock.h/winsock2.h redefinition errors.
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <fcntl.h>
#  include <poll.h>     // poll() -- the readiness scan behind the std::poll natives
#  include <unistd.h>
#  include <cerrno>
#  include <cstring>    // std::strerror
#  include <time.h>     // clock_gettime
#endif
#include "Platform.h"   // cross-platform macros + POSIX socket/SEH stubs

#include "Instruction.h"
#include "Context.h"
#include "VM_Resources.h"
#include "Heap.h"
#include "GlobalEnv.h"
#include "StringInterner.h"
#include "VM.h"
#include "Execute.h"
#include "NativeRegistry.h" // NativeId ids for build_native_table()
#include "Natives.h"        // build_native_table() declaration
#include "Interpreter.h"   // run_switch: the register-resident while{switch} dispatcher
#include "ValueCodec.h"    // vcodec::deep_eq is defined here, over Interpreter.h's worker

// ---- Portable socket type aliases -------------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
static void sock_close(socket_t s) { closesocket(s); }
#else
using socket_t = int;
static constexpr socket_t INVALID_SOCK = -1;
static void sock_close(socket_t s) { ::close(s); }
#endif

// ---- Portable readiness poll (std::poll natives) ----------------------------
// WSAPoll (Winsock 2.2) and POSIX poll() take the same {fd, events, revents} triple with the same
// meaning for the flags used here; only the spelling differs, including the event masks -- Winsock
// wants the *NORM variants, POSIX names the plain ones.
//
// KNOWN Windows limitation, deliberately not worked around: WSAPoll does not report a FAILED
// connection through POLLERR. It does not bite here because a connection is still established by
// the blocking tcpConnect (which has its own connect timeout) and only then switched to
// non-blocking -- stage 1 has no non-blocking connect.
#ifdef _WIN32
using pollfd_t = WSAPOLLFD;
static int sock_poll(pollfd_t* fds, unsigned n, int timeout_ms) { return WSAPoll(fds, n, timeout_ms); }
static constexpr short POLL_READ  = POLLRDNORM;
static constexpr short POLL_WRITE = POLLWRNORM;
#else
using pollfd_t = struct pollfd;
static int sock_poll(pollfd_t* fds, unsigned n, int timeout_ms) { return ::poll(fds, n, timeout_ms); }
static constexpr short POLL_READ  = POLLIN;
static constexpr short POLL_WRITE = POLLOUT;
#endif

// Writing to a socket whose peer has already closed raises SIGPIPE on BSD-derived systems (macOS),
// and SIGPIPE's default action TERMINATES the process -- a silent kill, not a returned error. With
// one short-lived connection per program this was nearly unreachable; under an event loop, peers
// disconnecting mid-write are routine, so it is suppressed at the source on every socket the
// registry takes ownership of. Windows has no such signal. Linux offers no SO_NOSIGPIPE and uses
// the per-call MSG_NOSIGNAL below instead.
static void sock_suppress_sigpipe(socket_t s) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<char*>(&on), sizeof(on));
#else
    (void)s;
#endif
}

#ifdef MSG_NOSIGNAL
static constexpr int SOCK_SEND_FLAGS = MSG_NOSIGNAL;  // Linux: suppress SIGPIPE per send() call
#else
static constexpr int SOCK_SEND_FLAGS = 0;             // Windows: no signal; macOS: SO_NOSIGPIPE above
#endif

// TCP socket registry for the std::net natives -- defined here (before execute()) so execute() can
// hold a stack-local instance whose destructor closes any socket still open at teardown. A socket is
// exposed to Skarn as a small Int DESCRIPTOR (an index into `socks`), never a raw OS SOCKET (a 64-bit
// kernel handle that need not fit a 48-bit Int). NOT a GC root (integer handles, no Values). The
// tcp* native implementations live further down, next to build_native_table().
//
// A DESCRIPTOR IS A SLOT PLUS A GENERATION: `slot | gen << SLOT_BITS`. A slot is reused once its
// socket is closed or handed to another isolate, and its generation then moves on, so a STALE
// descriptor -- a TcpConn kept after close(), or after handOff() -- no longer matches and is refused,
// instead of silently reaching whatever connection took the slot next. (Before generations, the
// next accept() reused the slot at once, which made "a socket used after it was handed off" an
// alias of a stranger's connection rather than an error.) The slot keeps one bit about how its
// previous generation ended, for the message.
struct NetRegistry {
    static constexpr int     SLOT_BITS = 24;
    static constexpr int64_t SLOT_MASK = (int64_t{1} << SLOT_BITS) - 1;
    struct Slot {
        socket_t s           = INVALID_SOCK;
        int64_t  gen         = 0;
        bool     prev_handed = false;   // the previous generation ended by a hand-off
    };
    std::vector<Slot> slots;
    ~NetRegistry() {
        for (const Slot& sl : slots)
            if (sl.s != INVALID_SOCK) sock_close(sl.s);
    }
    int64_t add(socket_t s) {
        size_t i = 0;
        while (i < slots.size() && slots[i].s != INVALID_SOCK) ++i;
        if (i == slots.size()) slots.push_back(Slot{});
        slots[i].s = s;
        return static_cast<int64_t>(i) | (slots[i].gen << SLOT_BITS);
    }
    // The live slot a descriptor names, or null (stale, never issued, or negative).
    Slot* live(int64_t fd) {
        if (fd < 0) return nullptr;
        const auto i = static_cast<size_t>(fd & SLOT_MASK);
        if (i >= slots.size() || slots[i].gen != (fd >> SLOT_BITS) || slots[i].s == INVALID_SOCK)
            return nullptr;
        return &slots[i];
    }
    socket_t get(int64_t fd) { Slot* sl = live(fd); return sl ? sl->s : INVALID_SOCK; }
    void release(Slot& sl, bool handed) {
        sl.s = INVALID_SOCK;
        sl.prev_handed = handed;
        sl.gen = (sl.gen + 1) & ((int64_t{1} << 23) - 1);   // stays within a positive 48-bit Int
    }
    void drop(int64_t fd) { if (Slot* sl = live(fd)) release(*sl, false); }
    // Detach a socket for a hand-off to another isolate: the socket leaves this registry (it is not
    // closed), and the descriptor goes stale. INVALID_SOCK if `fd` is not a live descriptor.
    socket_t hand_off(int64_t fd) {
        Slot* sl = live(fd);
        if (!sl) return INVALID_SOCK;
        const socket_t s = sl->s;
        release(*sl, true);
        return s;
    }
    // Why a descriptor is refused -- the text after "tcpX: ".
    std::string invalid_reason(int64_t fd) const {
        if (fd >= 0) {
            const auto i = static_cast<size_t>(fd & SLOT_MASK);
            const int64_t gen = fd >> SLOT_BITS;
            if (i < slots.size() && slots[i].prev_handed &&
                ((gen + 1) & ((int64_t{1} << 23) - 1)) == slots[i].gen)
                return "socket was handed to another actor";
        }
        return "invalid socket";
    }
};

// =============================================================================
// Tasks and actors -- one runtime for both (the "isolates" of concurrency stage 2).
//
// An ISOLATE is a second execute() on its own thread, with its own Heap, interner and stacks, over
// the program image of its WORLD. A TASK runs one function once and hands back one value (fork-
// join: rawSpawn / rawJoin / rawTaskTake). An ACTOR runs a function that loops on a MAILBOX until
// it is told to stop (rawSpawnActor / rawSend / rawReceive). Both are records in the same world.
//
// A WORLD is everything started, directly or not, under one ROOT execute() -- the call a driver
// makes. Ids are world-wide, so an actor's id means the same in every isolate of the world, which
// is what lets an address travel inside a message. The root owns the world as a stack local and
// destroys it before it returns, on the fault path too: every actor is sent Stop, and every
// thread still running is joined. So no isolate outlives the image it runs (the root's tables,
// World::image), at the price that a root whose actors ignore Stop -- or whose tasks never end --
// waits for them. Nothing can be cancelled.
//
// Nothing is shared between two heaps: every value crosses as a value-codec buffer, encoded in
// the sender's heap and decoded in the receiver's.
//
// An actor may own more than one INBOX: its main one, whose id is the actor's, and extra ones it
// makes (rawNewInbox), each with its own world-wide id -- typically one per request whose reply it
// waits for. Only the owner receives from an inbox; the end of the owner closes them all. Any inbox
// may be BOUNDED: then a send to it waits while it is full (back-pressure).
// =============================================================================

// One message in a mailbox. MAIL_EXITED reports that an actor this isolate started has FAULTED
// (v1 reports crashes only, to the starter). MAIL_STOP is delivered to every actor when the world
// ends.
enum : uint8_t { MAIL_NONE = 0, MAIL_MSG = 1, MAIL_EXITED = 2, MAIL_STOP = 3 };
struct Mail {
    uint8_t              kind = MAIL_NONE;
    std::vector<uint8_t> buf;          // MAIL_MSG: the message, a value-codec buffer
    int64_t              from = 0;     // MAIL_EXITED: the actor that ended
    std::string          reason;       // MAIL_EXITED: its fault message
};

// One inbox's queue: an actor's main inbox (its id is the actor's), the root's (id 0), or an extra
// inbox an actor or the root made with rawNewInbox. Only its OWNER receives from it.
//   * `dead` is set when the inbox is closed -- by rawCloseInbox or because its owner ended -- and
//     from then on a send is refused (the sender learns it from rawSend's Bool).
//   * `capacity` bounds the MESSAGES in the queue (0 = unbounded). Exit reports and Stop are
//     system mail and always enter, so a full inbox can still be stopped and told of a crash. A send
//     to a full inbox waits on `space` until a receive makes room, the inbox dies, or the world ends
//     (`released`) -- the last so that no sender blocked here can hold up the world's final join.
//   * `stop_seen`: once Stop has been received, every further receive answers Stop again, so any
//     loop ends.
//   * A SLOT (rawNewSlot) is a mailbox made BEFORE its actor and outliving it: an address that
//     survives a restart. When its actor ends it is VACATED instead of closed -- it keeps its id and
//     stays in the registry, so a send during the gap waits in it for the next actor. Only
//     rawReleaseSlot ends it.
struct Mailbox {
    enum Offer { SENT, FULL, GONE };
    static constexpr int64_t NO_OWNER = -1;      // a slot between two actors
    std::mutex              m;
    std::condition_variable cv;                  // a receiver waits for mail
    std::condition_variable space;               // a sender waits for room (bounded inboxes only)
    std::deque<Mail>        q;
    int64_t                 owner     = 0;       // the isolate that receives from it
    size_t                  capacity  = 0;       // messages; 0 = unbounded
    size_t                  messages  = 0;       // MAIL_MSG entries in q
    bool                    dead      = false;
    bool                    slot      = false;   // an address that outlives its actor (rawNewSlot)
    bool                    released  = false;   // the world is ending: a full inbox refuses
    bool                    stopped   = false;   // Stop has been queued
    bool                    stop_seen = false;   // ... and received
    // System mail (MAIL_EXITED, MAIL_STOP): never waits, ignores the capacity.
    bool put(Mail&& mail) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (dead) return false;
            if (mail.kind == MAIL_STOP) { if (stopped) return true; stopped = true; }
            q.push_back(std::move(mail));
        }
        cv.notify_one();
        return true;
    }
    // A message. `wait`: block while the inbox is full (rawSend); otherwise answer FULL (rawTrySend).
    Offer offer(Mail&& mail, bool wait) {
        {
            std::unique_lock<std::mutex> lk(m);
            if (capacity != 0 && wait)
                space.wait(lk, [&] { return dead || released || messages < capacity; });
            if (dead) return GONE;
            if (capacity != 0 && messages >= capacity) return wait ? GONE : FULL;
            q.push_back(std::move(mail));
            ++messages;
        }
        cv.notify_one();
        return SENT;
    }
    // Closing: refuse every later send, drop what is queued, and wake anyone waiting on either side.
    void close() {
        {
            std::lock_guard<std::mutex> lk(m);
            dead = true;
            q.clear();
            messages = 0;
        }
        cv.notify_all();
        space.notify_all();
    }
    void release() {
        { std::lock_guard<std::mutex> lk(m); released = true; }
        space.notify_all();
    }
    // A slot whose actor ended: it keeps its id and stays reachable, but what the actor did not read
    // is dropped -- re-delivering the message that crashed it would crash its successor too. A sender
    // waiting for room goes on, into the queue the next actor will read.
    void vacate() {
        {
            std::lock_guard<std::mutex> lk(m);
            owner    = NO_OWNER;
            q.clear();
            messages = 0;
        }
        space.notify_all();
    }
    // Ending a slot for good: no later send gets in, and an actor still running in it is told to
    // stop. NOT close(), which clears the queue and would drop that very Stop.
    void retire() {
        {
            std::lock_guard<std::mutex> lk(m);
            dead     = true;
            q.clear();
            messages = 0;
            if (owner != NO_OWNER && !stopped) { stopped = true; q.push_back(Mail{ MAIL_STOP, {}, 0, {} }); }
        }
        cv.notify_all();
        space.notify_all();
    }
};

// One task or actor. The fields below `joined` belong to a TASK: written by its thread, read by
// its starter only after joining it, so the join is the synchronization for them.
struct Isolate {
    int64_t                  id      = 0;
    int64_t                  starter = 0;       // the isolate that started it (0 = the root)
    bool                     actor   = false;
    std::shared_ptr<Mailbox> mailbox;           // actors only: the main inbox
    int64_t                  mailbox_id = 0;    // its id: the actor's own, or a slot's (rawSpawnInto)
    std::thread              thread;
    std::mutex               join_m;            // one joiner at a time: the starter, or the world
    bool                     thread_joined = false;
    bool                     claimed = false;   // a task: rawJoin has been called (once)
    std::vector<uint8_t>     input;             // the argument, encoded in the starter's heap
    std::vector<uint8_t>     output;            // a task's result, encoded in its heap (iff ok)
    std::string              error;             // a task's fault message (iff !ok)
    std::string              printed;           // what a task printed; written out at join
    bool                     ok    = false;
    bool                     taken = false;
    void join_thread() {
        std::lock_guard<std::mutex> lk(join_m);
        if (!thread_joined && thread.joinable()) thread.join();
        thread_joined = true;
    }
};

struct World;

// The output of the root and of every actor goes to the ROOT's stream, a LINE at a time: PRINTLN
// is two stream calls (text, newline), so unsynchronized writers would split each other's lines.
// A task does not use this -- it buffers everything and hands it over at join, in join order.
// Before the first actor starts, the root's writes pass straight through, so an ordinary program
// (and a prompt printed without a newline before reading stdin) behaves exactly as before.
class LineForwardBuf : public std::streambuf {
public:
    LineForwardBuf(World* w, bool always_lines) : world_(w), always_lines_(always_lines) {}
    ~LineForwardBuf() override { flush_partial(); }
    void flush_partial();
protected:
    int_type        overflow(int_type c) override;
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    int             sync() override;
private:
    void emit(const char* s, size_t n);
    World*      world_;
    bool        always_lines_;
    std::string line_;
};

struct World {
    const ProgramImage*       image  = nullptr;   // the ROOT's image: what every isolate runs
    std::ostream*             target = nullptr;   // the root's real output stream
    std::mutex                out_m;              // one line at a time into `target`
    std::atomic<bool>         actors_started{ false };
    LineForwardBuf            root_buf{ this, false };
    std::ostream              root_out{ &root_buf };
    std::mutex                m;                  // guards everything below
    std::unordered_map<int64_t, std::shared_ptr<Isolate>> isolates;
    int64_t                   next_id  = 1;       // 0 is the root
    std::shared_ptr<Mailbox>  main_mailbox = std::make_shared<Mailbox>();
    // Every open inbox by id: the root's (0), each actor's main inbox (under the actor's id) and the
    // extra ones (ids from the same counter as the isolates, so one id names one thing). A closed
    // inbox leaves this map, so a send to it finds nothing.
    std::unordered_map<int64_t, std::shared_ptr<Mailbox>> mailboxes{ { 0, main_mailbox } };
    bool                      main_inbox_taken = false;
    bool                      stopping = false;
    // Sockets in transit between isolates (rawHandOff -> rawTake): ticket -> OS socket. A socket here
    // belongs to no isolate's NetRegistry; whatever is never taken is closed when the world ends.
    std::unordered_map<int64_t, socket_t> handoffs;
    int64_t                   next_ticket = 1;

    std::shared_ptr<Isolate> find(int64_t id) {
        std::lock_guard<std::mutex> lk(m);
        auto it = isolates.find(id);
        return it == isolates.end() ? nullptr : it->second;
    }
    // The open inbox an id addresses; null for anything else (a task, a closed inbox, a stranger).
    std::shared_ptr<Mailbox> mailbox_of(int64_t id) {
        std::lock_guard<std::mutex> lk(m);
        auto it = mailboxes.find(id);
        return it == mailboxes.end() ? nullptr : it->second;
    }
    // Registers a new inbox for `owner` under `id`. Caller holds `m`. An actor's inbox made while
    // the world ends gets Stop at once, like an actor born then.
    void add_mailbox_locked(int64_t id, const std::shared_ptr<Mailbox>& box) {
        mailboxes.emplace(id, box);
        if (stopping && box->owner != 0) { box->put(Mail{ MAIL_STOP, {}, 0, {} }); box->release(); }
    }
    // Closes one inbox, or every inbox an isolate owns (its end).
    void close_mailbox(int64_t id) {
        std::shared_ptr<Mailbox> box;
        {
            std::lock_guard<std::mutex> lk(m);
            auto it = mailboxes.find(id);
            if (it == mailboxes.end()) return;
            box = it->second;
            mailboxes.erase(it);
        }
        box->close();
    }
    // Every inbox an isolate owns, right now (rawStopActor: an actor may be waiting on any of them).
    std::vector<std::shared_ptr<Mailbox>> mailboxes_of(int64_t owner) {
        std::vector<std::shared_ptr<Mailbox>> boxes;
        std::lock_guard<std::mutex> lk(m);
        for (auto& [id, box] : mailboxes) if (box->owner == owner) boxes.push_back(box);
        return boxes;
    }
    // Every inbox an isolate owns, at its end: closed -- except a SLOT, which is vacated and stays
    // in the registry, so its address survives until the next actor is started into it.
    void close_mailboxes_of(int64_t owner) {
        std::vector<std::shared_ptr<Mailbox>> boxes;
        std::vector<std::shared_ptr<Mailbox>> slots;
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto it = mailboxes.begin(); it != mailboxes.end();) {
                if (it->second->owner != owner) { ++it; continue; }
                if (it->second->slot) { slots.push_back(it->second); ++it; }
                else { boxes.push_back(it->second); it = mailboxes.erase(it); }
            }
        }
        for (auto& b : boxes) b->close();
        for (auto& s : slots) s->vacate();
    }
    // The end of the world: Stop into every actor's inboxes (an actor may be waiting on any of them),
    // release every sender blocked on a full inbox, then join every thread -- repeatedly, because an
    // actor may still start others while it winds down (those get Stop at birth, see spawn).
    ~World() {
        // The root's own partial line was written BEFORE anything the actors print while they stop;
        // without this it would come out after them, when root_buf is destroyed.
        root_out.flush();
        {
            std::lock_guard<std::mutex> lk(m);
            stopping = true;
            for (auto& [id, box] : mailboxes) {
                if (box->owner != 0) box->put(Mail{ MAIL_STOP, {}, 0, {} });
                box->release();
            }
        }
        for (;;) {
            std::shared_ptr<Isolate> next;
            {
                std::lock_guard<std::mutex> lk(m);
                for (auto& [id, iso] : isolates)
                    if (!iso->thread_joined) { next = iso; break; }
            }
            if (!next) break;
            next->join_thread();
        }
        close_mailboxes_of(0);   // the root's own inboxes: every actor has ended, nobody sends
        // Every isolate has ended, so nobody can take these any more.
        for (auto& [ticket, s] : handoffs) sock_close(s);
    }
};

void LineForwardBuf::emit(const char* s, size_t n) {
    std::lock_guard<std::mutex> lk(world_->out_m);
    world_->target->write(s, static_cast<std::streamsize>(n));
}
void LineForwardBuf::flush_partial() {
    if (!line_.empty()) { emit(line_.data(), line_.size()); line_.clear(); }
    std::lock_guard<std::mutex> lk(world_->out_m);
    world_->target->flush();
}
std::streamsize LineForwardBuf::xsputn(const char* s, std::streamsize n) {
    if (!always_lines_ && !world_->actors_started.load(std::memory_order_acquire)) {
        // No actor yet: nothing to interleave with, so write through at once. Still under the
        // lock -- a task on another thread may start the first actor at any moment.
        if (!line_.empty()) { emit(line_.data(), line_.size()); line_.clear(); }
        emit(s, static_cast<size_t>(n));
        return n;
    }
    line_.append(s, static_cast<size_t>(n));
    size_t nl = line_.rfind('\n');
    if (nl != std::string::npos) {                     // every complete line in one write
        emit(line_.data(), nl + 1);
        line_.erase(0, nl + 1);
    }
    return n;
}
LineForwardBuf::int_type LineForwardBuf::overflow(int_type c) {
    if (c != traits_type::eof()) { const char ch = traits_type::to_char_type(c); xsputn(&ch, 1); }
    return traits_type::not_eof(c);
}
// An explicit flush writes out a partial line too: a prompt printed without a newline before a read
// must appear even while actors run (the stdin natives flush the output first).
int LineForwardBuf::sync() {
    flush_partial();
    return 0;
}

// This execution's place in its world (VM::isolate). The root has id 0 and gets its main inbox only
// through rawMainInbox; an actor has its own from the start; a task has none and may make none.
// `inboxes` are the open inboxes this isolate owns, by id -- a copy of its entries in the world's
// map, so a receive finds its queue without taking the world's lock (usually one or two entries).
// `current` is the mail rawReceive took last, which rawMailMsg / rawMailFrom / rawMailReason read.
struct IsolateLocal {
    World*                   world = nullptr;
    int64_t                  id    = 0;
    int64_t                  main_box = 0;   // the id of its main inbox: its own, or the slot it runs in
    bool                     actor = false;
    std::vector<std::pair<int64_t, std::shared_ptr<Mailbox>>> inboxes;
    Mail                     current;
    Mailbox* inbox(int64_t box_id) const {
        for (const auto& [i, b] : inboxes) if (i == box_id) return b.get();
        return nullptr;
    }
};

// =============================================================================
// execute -- sets up VM_Resources + Context, runs bytecode, returns resources
// so the caller can inspect registers after HALT. Declared in Execute.h (which
// carries the default arguments); this definition must not repeat them.
// =============================================================================
// The three GC-rooted Value pools below (string literals, the TO_STRING display pool,
// and const arrays) use RootedValuePool, which lives in Heap.h -- it is a general
// rooting utility, not an execute() detail, and ValueCodec.h rebuilds object graphs
// with the same one.

VM_Resources execute(const std::vector<uint32_t>& bytecode,
                     Heap* heap,
                     GlobalEnv* global_env,
                     StringInterner* string_interner,
                     uint8_t top_frame_size,
                     const std::vector<Value>* const_pool,
                     const std::vector<StructType>* struct_types,
                     const std::vector<std::string>* string_literals,
                     const std::vector<std::string>* atom_names,
                     const std::vector<FnInfo>* fn_table,
                     std::ostream* out,
                     const std::vector<uint16_t>* trait_table,
                     uint32_t trait_table_width,
                     uint32_t trait_method_count,
                     const std::vector<uint32_t>* line_table,
                     const std::vector<std::string>* function_names,
                     const std::vector<uint32_t>* column_table,
                     const std::vector<NativeFunc>* native_table,
                     const std::vector<std::string>* script_args,
                     std::istream* in,
                     const std::vector<std::string>* function_modules,
                     const std::vector<std::vector<Value>>* const_arrays,
                     const TaskEntry* task) {
    std::vector<uint32_t> padded = bytecode;
    padded.resize(bytecode.size() + 3,
        Instruction::J(static_cast<uint8_t>(OpCode::HALT)).raw);

    // The "current VM instance" -- gathers the shared state the execution needs
    // behind one pointer reached through Context.vm. This replaces the three TLS
    // hooks and their save/restore guards: because vm rides in the per-execution
    // Context, nesting is automatically correct without any global state. The
    // local outlives the run_switch() call below, so ctx.vm stays valid throughout.
    VM vm;
    vm.heap            = heap;
    vm.globals         = global_env;
    vm.interner        = string_interner;
    vm.const_pool        = const_pool ? const_pool->data() : nullptr;
    vm.const_pool_size   = const_pool ? const_pool->size() : 0;
    vm.struct_types      = struct_types ? struct_types->data() : nullptr;
    vm.struct_type_count = struct_types ? struct_types->size() : 0;
    vm.fn_table          = fn_table ? fn_table->data() : nullptr;
    vm.fn_table_size     = fn_table ? fn_table->size() : 0;
    vm.trait_table         = trait_table ? trait_table->data() : nullptr;
    vm.trait_table_width   = trait_table_width;
    vm.trait_method_count  = trait_method_count;
    vm.native_table        = native_table ? native_table->data() : nullptr;
    vm.native_table_size   = native_table ? native_table->size() : 0;
    vm.script_args         = script_args;   // process context for the args() native
    // TCP socket registry (std::net natives). A stack-local whose destructor closes any socket
    // still open when execute() returns (incl. the VmFault path) -- the safety net behind tcpClose.
    NetRegistry net_registry;
    vm.net                 = &net_registry;
    // Tasks and actors: the image this call runs, built from its own arguments, and its WORLD. A
    // root (no `task`) owns a new world; a task or actor joins the one it was started in.
    // Declared in this order so the world is destroyed FIRST -- its destructor stops every actor
    // and joins every thread while the image (and the caller's vectors) are still alive.
    const ProgramImage image{ &bytecode, const_pool, struct_types, string_literals, atom_names,
                              fn_table, trait_table, trait_table_width, trait_method_count,
                              line_table, function_names, column_table, native_table, script_args,
                              function_modules, const_arrays };
    std::optional<World> own_world;
    IsolateLocal         local;
    if (task) {
        local.world        = task->world;
        local.id           = task->isolate->id;
        local.actor        = task->isolate->actor;
        local.main_box     = task->isolate->mailbox_id;
        if (task->isolate->mailbox)
            local.inboxes.emplace_back(task->isolate->mailbox_id, task->isolate->mailbox);
        vm.task_input      = task->input;
        vm.task_input_size = task->input_size;
        vm.task_entry_pc   = task->entry_pc;
    } else {
        own_world.emplace();
        own_world->image  = &image;
        own_world->target = out ? out : &std::cout;
        local.world       = &*own_world;
    }
    vm.image   = &image;
    vm.isolate = &local;

    // PRINT / PRINTLN sink: caller-supplied stream, or std::cout by default. A caller
    // (tests, a future REPL) can capture output by passing its own std::ostream. A root writes
    // through its world's line buffer, which passes everything straight on until an actor runs
    // (see LineForwardBuf); an isolate's `out` comes from its runner.
    vm.out               = task ? (out ? out : &std::cout) : &own_world->root_out;
    // stdin sink for readLine / readAllStdin: caller-supplied stream, or std::cin by
    // default. A caller (tests, a REPL) can feed input by passing its own std::istream.
    vm.in                = in ? in : &std::cin;
    // Serious-fault diagnostics (Phase 1): the code origin (instruction 0) so a fault
    // ip maps to an instruction index, plus the per-instruction line table and the
    // per-fn-id name table. Cold-path only (raise_located); not GC roots.
    vm.code_base            = reinterpret_cast<const uint8_t*>(padded.data());
    vm.line_table           = line_table ? line_table->data() : nullptr;
    vm.line_table_size      = line_table ? line_table->size() : 0;
    vm.column_table         = column_table ? column_table->data() : nullptr;
    vm.column_table_size    = column_table ? column_table->size() : 0;
    vm.function_names       = function_names ? function_names->data() : nullptr;
    vm.function_names_size  = function_names ? function_names->size() : 0;
    vm.function_modules      = function_modules ? function_modules->data() : nullptr;
    vm.function_modules_size = function_modules ? function_modules->size() : 0;

    // The constant pool is NOT scanned as a GC root, so under the copying
    // collector it must never hold a heap pointer -- an unforwarded TAG_PTR
    // there would dangle after the first collection. Today the pool only holds
    // immediates (doubles / full 64-bit Value bit patterns). If pooled heap
    // pointers are ever wanted, register the pool as a forwarded root source
    // (see Heap::forward / forward_vm_external_roots).
#ifndef NDEBUG
    if (const_pool) {
        for (const Value& v : *const_pool)
            assert(!v.isPtr() && "constant pool must not hold heap pointers");
    }
#endif

    // A task or actor starts with small stacks (they grow on demand, see VM_Resources); the root
    // keeps the historical initial commit.
    VM_Resources res = task ? VM_Resources(VM_Resources::ISOLATE_REG_INIT_SIZE,
                                           VM_Resources::ISOLATE_RET_FRAME_COUNT)
                            : VM_Resources();
    // The parallel closure stack lives in VM_Resources; publish its base into the VM
    // so run_switch can push/pop saved caller closures and the collector can scan the
    // live entries (current_closure starts Undefined -- the top-level activation
    // captures nothing). See VM.h and forward_vm_external_roots below.
    vm.closure_stack_base = res.get_closure_base();
    // Enable growable stacks: run_switch's soft-check at the three non-tail call ops grows
    // the register/return stacks on demand via this back-pointer (reserve-big/commit-
    // incrementally, fixed bases). Only execute() sets it; the hand-built Contexts in
    // vm_tests leave vm.resources null, so their stacks stay at the INITIAL commit and an
    // overflow faults on reserved-but-uncommitted memory (the old fixed-cap behavior).
    vm.resources = &res;

    Context ctx;
    // A task starts at its entry stub, appended after the program's own code; everything
    // else starts at instruction 0 (the top-level code).
    assert((!task || task->entry_pc < bytecode.size()) && "task entry outside the code");
    ctx.ip                 = reinterpret_cast<const uint8_t*>(padded.data() + (task ? task->entry_pc : 0));
    ctx.window_ptr         = res.get_reg_base();
    ctx.vm                 = &vm;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base();
    // The Debug overflow assert at the three call ops (rsp < ret_stack_limit) means the
    // true RESERVED cap in the growable path -- the soft-check grows (or raises a clean
    // located "stack overflow") before rsp ever reaches it, so the assert never false-fires
    // when the return stack legitimately grows past its initial commit. (The vm_tests
    // helpers keep + RET_FRAME_COUNT -- their real, growth-disabled fault point.)
    ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_MAX_FRAMES;
    ctx.frame_size_ptr     = res.get_frame_size_base();
    // Top-level frame size: it plays the same dual role as any function's frame
    // size -- the GC scan count for the top-level register frame AND the slide the
    // first CALL uses (op_call slides by the CALLER's frame size). It must therefore
    // be the boundary between the top-level's own locals and its outgoing-argument
    // window: cover every top-level register that can hold a heap pointer across a
    // CALL / GC-triggering allocation, and equal the register index where outgoing
    // arguments start. Defaults to 0 (top-level performs no CALL and holds no heap
    // pointers); callers pass an explicit value otherwise. There is no auto-computed
    // helper -- see "Calling convention" in docs/VirtualMachine.md.
    ctx.current_frame_size = top_frame_size;

    // Pre-intern string literals into a GC-ROOTED pool before running. Strings are
    // heap objects (TAG_PTR), so they cannot ride the pointer-free constant pool;
    // instead each literal text is interned once here and its pool slot registered
    // as a GC root, so the moving collector rewrites the pointer in place. LOAD_STR
    // then just indexes ctx->vm->string_pool. This runs AFTER ctx is fully set up,
    // because interning can trigger a collection that walks ctx's register frames
    // (harmless here: the fresh top frame holds only Undefined slots).
    RootedValuePool str_pool;
    if (string_literals && !string_literals->empty()) {
        assert(heap && string_interner &&
               "string literals require both a heap and a string interner");
        str_pool.heap = heap;
        str_pool.slots.resize(string_literals->size());  // fixed size -> stable &slot addresses
        for (Value& s : str_pool.slots)
            heap->add_root(&s);                           // root BEFORE interning can collect
        for (size_t i = 0; i < string_literals->size(); ++i)
            str_pool.slots[i] = string_interner->intern((*string_literals)[i], *heap, &ctx);
        vm.string_pool      = str_pool.slots.data();
        vm.string_pool_size = str_pool.slots.size();
    }

    // Pre-intern TO_STRING's display strings into a second GC-ROOTED pool: the atom
    // names (each prefixed with ':') indexed by atom id, plus the three constants
    // true/false/nil. These are bounded, compile-time-known sets, so they are
    // materialized once here -- TO_STRING then does a rooted-pool lookup for atoms /
    // bool / nil (no allocation); only its numeric branch allocates. Same rooting
    // discipline as the string-literal pool above. Built whenever a caller passes an
    // atom_names table (the compiler always does, possibly empty, so the true/false/nil
    // constants are available to every compiled program); legacy callers that pass
    // nullptr get no pool and can only TO_STRING numbers/strings.
    RootedValuePool atom_pool;
    if (atom_names && heap && string_interner) {
        const size_t n = atom_names->size();
        atom_pool.heap = heap;
        atom_pool.slots.resize(n + 3);                // [0,n) atoms, then true/false/nil
        for (Value& s : atom_pool.slots)
            heap->add_root(&s);                       // root BEFORE interning can collect
        for (size_t i = 0; i < n; ++i)
            atom_pool.slots[i] = string_interner->intern(":" + (*atom_names)[i], *heap, &ctx);
        atom_pool.slots[n + 0] = string_interner->intern("true",  *heap, &ctx);
        atom_pool.slots[n + 1] = string_interner->intern("false", *heap, &ctx);
        atom_pool.slots[n + 2] = string_interner->intern("nil",   *heap, &ctx);
        vm.atom_pool      = atom_pool.slots.data();
        vm.atom_pool_size = n;
        vm.str_true       = &atom_pool.slots[n + 0];
        vm.str_false      = &atom_pool.slots[n + 1];
        vm.str_nil        = &atom_pool.slots[n + 2];
    }

    // Build the const-array literals into a GC-ROOTED pool, once, before running. Each
    // const is one KIND_ARRAY whose elements are scalar immediates (Int/Double/Bool), so --
    // unlike the string pool -- there is NO per-element allocation and thus no GC mid-fill;
    // the only safepoint is the per-array alloc_slots_gc, and every already-built array is
    // held by its (rooted) pool slot across it, so the collector rewrites it in place.
    // LOAD_CONST_ARRAY then just indexes ctx->vm->const_array_pool. Same rooting discipline
    // as the string-literal pool. The shared single instance is sound because the checker
    // keeps a const array non-escaping (read-only).
    RootedValuePool carr_pool;
    if (const_arrays && !const_arrays->empty()) {
        assert(heap && "const arrays require a heap");
        carr_pool.heap = heap;
        carr_pool.slots.resize(const_arrays->size());   // fixed size -> stable &slot addresses
        for (Value& s : carr_pool.slots)
            heap->add_root(&s);                          // root BEFORE any alloc can collect
        for (size_t i = 0; i < const_arrays->size(); ++i) {
            const std::vector<Value>& elems = (*const_arrays)[i];
            GcObject* arrObj = heap->alloc_slots_gc(
                GcObject::KIND_ARRAY, static_cast<uint32_t>(elems.size()), &ctx);
            // Scalar immediates only -> no allocation between here and the last store, so
            // arrObj cannot move before we publish it into the rooted slot below.
            Value* dst = arrObj->slots();
            for (size_t j = 0; j < elems.size(); ++j)
                dst[j] = elems[j];
            carr_pool.slots[i] = Value::fromPtr(arrObj->payload());
        }
        vm.const_array_pool      = carr_pool.slots.data();
        vm.const_array_pool_size = carr_pool.slots.size();
    }

    run_switch(&ctx);
#ifdef VM_COUNT_OPS
    vm_prof::dump_op_counts();   // profiling build only; writes to stderr (see Interpreter.h)
#endif
    return res;
}

// The structural-equality seam for ValueCodec.h. value_deep_eq is TU-local to this file
// (it is `static` in Interpreter.h and inlined into the dispatch loop), so a test that
// wants to check a round trip cannot reach it -- and a comparison written next to the
// codec would be an oracle that shares the codec's assumptions, which is precisely the
// kind of differential that cannot fail. One forwarder keeps the real comparison
// available without touching Execute.h or the dispatch path.
namespace vcodec {
bool deep_eq(Value a, Value b, Context* ctx) { return value_deep_eq(a, b, ctx); }
} // namespace vcodec

// The decoupling seam that keeps Heap.h ignorant of VM's layout: the collector
// calls this to forward external strong roots -- the active interner AND the
// closure activation state (Milestone A). The closure stack is lockstep with the
// return stack, so its live depth is exactly the return-stack depth; scanning
// [0..ret_depth) plus current_closure forwards every live closure reference in
// place (each captured heap pointer is then reached transitively by the Cheney
// scan of the KIND_CLOSURE payload).
void forward_vm_external_roots(Context* ctx, Heap& heap) noexcept {
    VM* vm = ctx->vm;
    if (!vm)
        return;
    if (vm->interner)
        vm->interner->forward_roots(heap);

    // The innermost activation's closure (Undefined for non-closure activations --
    // forward() skips it as a non-pointer).
    heap.forward(&vm->current_closure);

    // The saved caller closures on the parallel closure stack.
    if (vm->closure_stack_base) {
        const size_t ret_depth =
            static_cast<size_t>(ctx->ret_stack_ptr - ctx->ret_stack_base);
        for (size_t i = 0; i < ret_depth; ++i)
            heap.forward(&vm->closure_stack_base[i]);
    }
}

// =============================================================================
// Built-in native functions (the native registry -- see Natives.h /
// NativeRegistry.h). They return ONLY heap-KIND values (Bytes / String / nil),
// never struct-type ids; the compiler wraps the result in Ok/Err. A native is
// called at a GC safepoint (CALL_NATIVE SYNC'd the cursors), so it may allocate
// -- but it must materialize any argument bytes into a HOST buffer BEFORE the
// allocation, since a collection can relocate the argument objects.
// =============================================================================

// Allocate a KIND_STRING error message and return it as a Value. Used for both
// the "String = error" convention and argument-validation failures.
static Value native_make_error(Context* ctx, const std::string& msg) noexcept {
    GcObject* o = ctx->vm->heap->alloc_string_gc(msg, ctx);
    return Value::fromPtr(o->payload());
}

// readFile(path) -> Bytes (success) | String (error message).
static Value native_read_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "readFile: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());   // host copy, before any alloc
    }
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return native_make_error(ctx, "could not open file: " + path);
    const std::string contents((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    // Build a KIND_BYTES from the raw bytes. Allocates (safepoint), but `contents`
    // is a host buffer, stable across a collection.
    Value result;
    bytes_from_str(ctx, &result, contents);
    return result;
}

// writeFile(path, buffer) -> nil (success) | String (error message).
// v1: `buffer` must be a KIND_BYTES (a String source is deliberately NOT accepted
// yet.
static Value native_write_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 2 || !is_string(args[0]))
        return native_make_error(ctx, "writeFile: path must be a string");
    if (!args[1].isPtr() ||
        GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "writeFile: second argument must be a byte buffer");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    // Materialize the buffer's live bytes [0, count) into a host string BEFORE any
    // alloc / file op (a later error alloc could otherwise relocate the backing).
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return native_make_error(ctx, "could not open file: " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f)
        return native_make_error(ctx, "could not write file: " + path);
    return Value::fromNil();
}

// A monotonic high-resolution clock reading in nanoseconds.
static int64_t qpc_now_ns() noexcept {
#ifdef _WIN32
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (c.QuadPart / freq.QuadPart) * 1000000000LL
         + ((c.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

// nanoTime() -> Int. A MONOTONIC counter, offset from the first call (Java
// System.nanoTime semantics: only differences are meaningful). The offset fits 48
// bit for ~39 h, then wraps via fromSigned48 (documented limit) -- absolute nanos
// since epoch would NOT fit, which is exactly why this is offset-from-start. Plain
// return kind -> no Ok/Err wrap. Does not allocate.
static Value native_nano_time(Value*, uint8_t, Context*) {
    static const int64_t origin = qpc_now_ns();
    return Value::fromSigned48(qpc_now_ns() - origin);
}

// millisTime() -> Int. Wall-clock milliseconds since the Unix epoch (~1.7e12 today,
// comfortably within MAX_48 ~ 1.4e14 -> good to ~year 6400). Plain return kind. Does
// not allocate.
static Value native_millis_time(Value*, uint8_t, Context*) {
    using namespace std::chrono;
    return Value::fromSigned48(static_cast<int64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()));
}

// rawOsId() -> Int. Which platform the program is RUNNING on: 0 Windows, 1 macOS, 2 anything
// else. Decided at compile time of this file, but read at run time by the program, so a .skbc
// answers for the machine that executes it, not the one that produced it. Plain return kind,
// total, does not allocate. The surface is the prelude's `currentOs() -> Os` (std/process.skn),
// which is what lets `sh` pick the platform's shell.
static Value native_os_id(Value*, uint8_t, Context*) {
#ifdef _WIN32
    return Value::fromSigned48(0);
#elif defined(__APPLE__)
    return Value::fromSigned48(1);
#else
    return Value::fromSigned48(2);
#endif
}

// args() -> Array[String] (Plain -- no Ok/Err wrap). Builds a fresh KIND_ARRAY of the
// driver-supplied command-line args (host std::strings in VM::script_args, stable across
// a collection). Each element string is a separate allocation (a safepoint that can move
// the array), so the array is ROOTED via heap->add_root while it is being filled -- the
// collector then rewrites `arr` in place and the already-stored strings survive as its
// (scanned) slots. Unfilled slots are memset-0 (a double, not a pointer) -> GC-safe.
static Value native_args(Value*, uint8_t, Context* ctx) {
    Heap* heap = ctx->vm->heap;
    const std::vector<std::string>* a = ctx->vm->script_args;
    const uint32_t n = a ? static_cast<uint32_t>(a->size()) : 0u;
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, n, ctx);
    Value arr = Value::fromPtr(arrObj->payload());
    if (n == 0) return arr;                       // empty array, no element allocations
    heap->add_root(&arr);                         // track moves during the string allocs
    for (uint32_t i = 0; i < n; ++i) {
        GcObject* s = heap->alloc_string_gc((*a)[i], ctx);   // may collect -> arr rewritten
        // No allocation between here and the store, so `s` cannot move before we root it
        // in the (up-to-date, via the root) array slot.
        GcObject::from_slots(arr.asPtr())->slots()[i] = Value::fromPtr(s->payload());
    }
    heap->remove_root(&arr);
    return arr;
}

// getEnv(name) -> String (present) | nil (absent). The compiler wraps this Some/None
// (NativeReturn::Option). Option has no error channel, so a non-string argument reads as None.
// Single heap alloc on the present path.
static Value native_get_env(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return Value::fromNil();
    std::string name;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        name.assign(o->bytes(), o->string_length());
    }
    std::string val;
#ifdef _WIN32
    char*  buf = nullptr;
    size_t sz  = 0;
    if (_dupenv_s(&buf, &sz, name.c_str()) != 0 || !buf) {
        std::free(buf);
        return Value::fromNil();
    }
    val.assign(buf, sz > 0 ? sz - 1 : 0);  // sz includes trailing NUL
    std::free(buf);
#else
    const char* raw = std::getenv(name.c_str());
    if (!raw) return Value::fromNil();
    val = raw;
#endif
    GcObject* s = ctx->vm->heap->alloc_string_gc(val, ctx);
    return Value::fromPtr(s->payload());
}

// fileExists(path) -> Bool (Plain -- no wrap). True for ANY filesystem entry (file or
// directory, `test -e` style); a non-string arg or a lookup error reads as false.
// Allocates nothing.
static Value native_file_exists(Value* args, uint8_t nargs, Context*) {
    if (nargs < 1 || !is_string(args[0])) return Value::fromBool(false);
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    return Value::fromBool(std::filesystem::exists(path, ec) && !ec);
}

// deleteFile(path) -> nil (success) | String (error message). A missing file is an Err
// (Rust remove_file semantics). std::filesystem::remove also deletes an empty directory;
// a non-empty directory / permission failure sets `ec` -> Err. Allocates only on the Err
// path (native_make_error); the path is materialized host-side before that alloc.
static Value native_delete_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "deleteFile: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec)       return native_make_error(ctx, "could not delete file: " + path +
                                                " (" + ec.message() + ")");
    if (!removed) return native_make_error(ctx, "no such file: " + path);
    return Value::fromNil();
}

// listDir(path) -> Array[String] (success) | String (error message). The compiler wraps
// this Ok/Err (NativeReturn::Result); the success result is a KIND_ARRAY, so the Ok/Err
// discriminator (String => Err) routes it to Ok(array). Elements are the LEAF names of the
// directory entries (filename(), ls-style; neither "." nor ".." are yielded by
// directory_iterator), in filesystem order (unspecified). Two phases: collect every name
// host-side FIRST (no VM alloc while iterating), then build the KIND_ARRAY exactly like
// native_args (root-then-fill, so a per-string collection can move the array in place).
static Value native_list_dir(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "listDir: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec), end;
    if (ec)
        return native_make_error(ctx, "could not list directory: " + path +
                                      " (" + ec.message() + ")");
    std::vector<std::string> names;                 // host-side, before any VM alloc
    for (; it != end; it.increment(ec)) {
        if (ec)
            return native_make_error(ctx, "could not list directory: " + path +
                                          " (" + ec.message() + ")");
        names.push_back(it->path().filename().string());
    }
    // Build the KIND_ARRAY -- same rooted-then-filled discipline as native_args.
    Heap*          heap = ctx->vm->heap;
    const uint32_t n    = static_cast<uint32_t>(names.size());
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, n, ctx);
    Value arr = Value::fromPtr(arrObj->payload());
    if (n == 0) return arr;                         // empty directory -> empty array
    heap->add_root(&arr);                           // track moves during the string allocs
    for (uint32_t i = 0; i < n; ++i) {
        GcObject* s = heap->alloc_string_gc(names[i], ctx);   // may collect -> arr rewritten
        GcObject::from_slots(arr.asPtr())->slots()[i] = Value::fromPtr(s->payload());
    }
    heap->remove_root(&arr);
    return arr;
}

// mkdir(path) -> nil (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result). create_directories (mkdir -p): creates any missing parent
// directories too, and an ALREADY-EXISTING directory is idempotent success (create_directories
// returns false with a clear ec in that case). A real failure (a path component is a file,
// permission denied, ...) sets ec -> Err. Allocates only on the Err path (native_make_error).
static Value native_make_dir(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "mkdir: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);  // false + clear ec == already existed (Ok)
    if (ec)
        return native_make_error(ctx, "could not create directory: " + path +
                                      " (" + ec.message() + ")");
    return Value::fromNil();
}

// appendFile(path, buffer) -> nil (success) | String (error message). The compiler wraps
// this Ok/Err (NativeReturn::Result). Like writeFile but opens the file in APPEND mode
// (std::ios::app), creating it if absent -- so repeated calls accumulate. `buffer` must be
// a KIND_BYTES (a String source is served by the pure-prelude appendTextFile wrapper). The
// buffer's live bytes are materialized host-side BEFORE the file op (a later error alloc
// could otherwise relocate the backing).
static Value native_append_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 2 || !is_string(args[0]))
        return native_make_error(ctx, "appendFile: path must be a string");
    if (!args[1].isPtr() ||
        GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "appendFile: second argument must be a byte buffer");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);
    }
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f)
        return native_make_error(ctx, "could not open file: " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f)
        return native_make_error(ctx, "could not write file: " + path);
    return Value::fromNil();
}

// isFile(path) -> Bool (Plain -- no wrap). True iff the path names a regular file (NOT a
// directory -- the finer complement of fileExists, which is `test -e`). A non-string arg or
// a lookup error reads as false. Allocates nothing.
static Value native_is_file(Value* args, uint8_t nargs, Context*) {
    if (nargs < 1 || !is_string(args[0])) return Value::fromBool(false);
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    return Value::fromBool(std::filesystem::is_regular_file(path, ec) && !ec);
}

// isDir(path) -> Bool (Plain -- no wrap). True iff the path names a directory. A non-string
// arg or a lookup error reads as false. Allocates nothing.
static Value native_is_dir(Value* args, uint8_t nargs, Context*) {
    if (nargs < 1 || !is_string(args[0])) return Value::fromBool(false);
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    return Value::fromBool(std::filesystem::is_directory(path, ec) && !ec);
}

// fileSize(path) -> Int (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result); the success payload is an IMMEDIATE Int (like parseInt), so the
// Ok/Err discriminator (String => Err) routes it to Ok(size). std::filesystem::file_size of
// a missing / non-regular path (a directory is implementation-defined) sets `ec` -> Err. A
// size beyond signed 48-bit (~140 TB) is also reported as an Err rather than silently
// truncated. Allocates only on the Err path.
static Value native_file_size(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "fileSize: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    if (ec)
        return native_make_error(ctx, "could not stat file: " + path +
                                      " (" + ec.message() + ")");
    constexpr std::uintmax_t MAX_48 = 140737488355327ULL;   // 2^47 - 1
    if (sz > MAX_48)
        return native_make_error(ctx, "fileSize: file too large for Int: " + path);
    return Value::fromSigned48(static_cast<int64_t>(sz));    // Ok payload (immediate)
}

// Materialize two KIND_STRING path arguments into host buffers BEFORE any alloc (the two-path file
// ops below share this). Returns false + writes an error Value into *err if either arg is not a string.
static bool native_two_paths(Value* args, uint8_t nargs, Context* ctx, const char* who,
                             std::string& from, std::string& to, Value* err) {
    if (nargs < 2 || !is_string(args[0]) || !is_string(args[1])) {
        *err = native_make_error(ctx, std::string(who) + ": both arguments must be strings");
        return false;
    }
    GcObject* a = GcObject::from_slots(args[0].asPtr());
    from.assign(a->bytes(), a->string_length());
    GcObject* b = GcObject::from_slots(args[1].asPtr());
    to.assign(b->bytes(), b->string_length());
    return true;
}

// rename(from, to) -> nil (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result). std::filesystem::rename OVERWRITES an existing destination and is atomic on
// the same filesystem; a cross-filesystem move (or a missing source / permission failure) sets `ec`
// and is passed through as an Err. Allocates only on the Err path.
static Value native_rename(Value* args, uint8_t nargs, Context* ctx) {
    std::string from, to; Value err;
    if (!native_two_paths(args, nargs, ctx, "rename", from, to, &err)) return err;
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
        return native_make_error(ctx, "could not rename " + from + " -> " + to +
                                      " (" + ec.message() + ")");
    return Value::fromNil();
}

// copyFile(from, to) -> nil (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result). Uses the std::filesystem::copy_file DEFAULT (copy_options::none): an
// ALREADY-EXISTING destination is an Err (no silent overwrite), as is a missing / non-regular source.
// Allocates only on the Err path.
static Value native_copy_file(Value* args, uint8_t nargs, Context* ctx) {
    std::string from, to; Value err;
    if (!native_two_paths(args, nargs, ctx, "copyFile", from, to, &err)) return err;
    std::error_code ec;
    std::filesystem::copy_file(from, to, ec);   // default: fail if `to` exists
    if (ec)
        return native_make_error(ctx, "could not copy " + from + " -> " + to +
                                      " (" + ec.message() + ")");
    return Value::fromNil();
}

// std::math natives -- thin libm wrappers (Plain return, no allocation, no safepoint). The compiler
// types the arg(s) as Double (an Int arg widens via I2D at the call boundary), but numAsDouble()
// promotes either kind defensively. IEEE semantics: a domain error yields NaN / +-inf (no trap).
#define MATH1(NAME, FN) \
    static Value NAME(Value* a, uint8_t, Context*) { return Value::fromDouble(FN(a[0].numAsDouble())); }
#define MATH2(NAME, FN) \
    static Value NAME(Value* a, uint8_t, Context*) { return Value::fromDouble(FN(a[0].numAsDouble(), a[1].numAsDouble())); }
#define MATHP(NAME, FN) \
    static Value NAME(Value* a, uint8_t, Context*) { return Value::fromBool(FN(a[0].numAsDouble())); }
MATH1(native_sqrt,  std::sqrt)
MATH1(native_cbrt,  std::cbrt)
MATH2(native_pow,   std::pow)
MATH2(native_hypot, std::hypot)
MATH1(native_exp,   std::exp)
MATH1(native_ln,    std::log)      // natural log (surface name `ln`)
MATH1(native_log2,  std::log2)
MATH1(native_log10, std::log10)
MATH1(native_sin,   std::sin)
MATH1(native_cos,   std::cos)
MATH1(native_tan,   std::tan)
MATH1(native_asin,  std::asin)
MATH1(native_acos,  std::acos)
MATH1(native_atan,  std::atan)
MATH2(native_atan2, std::atan2)
MATH1(native_fabs,  std::fabs)
MATHP(native_isnan, std::isnan)
MATHP(native_isinf, std::isinf)
#undef MATH1
#undef MATH2
#undef MATHP

// readLine() -> String (a line) | nil (EOF). The compiler wraps this Some/None
// (NativeReturn::Option): nil => None at EOF, else Some(line). Reads one line from the
// VM's input sink (ctx->vm->in, std::cin by default, redirectable for tests). getline strips
// the trailing '\n'; a trailing '\r' (CRLF from a pipe/binary stream) is stripped defensively.
// Zero-arg (dummy window base). Allocates one string on the Some path (safepoint); None
// allocates nothing.
static Value native_read_line(Value*, uint8_t, Context* ctx) {
    if (ctx->vm->out) ctx->vm->out->flush();   // a prompt printed without a newline must appear before the wait
    std::string line;
    if (!std::getline(*ctx->vm->in, line))
        return Value::fromNil();                    // EOF / stream error -> None
    if (!line.empty() && line.back() == '\r')
        line.pop_back();                            // CRLF -> LF (defensive)
    GcObject* s = ctx->vm->heap->alloc_string_gc(line, ctx);
    return Value::fromPtr(s->payload());
}

// readAllStdin() -> String. Reads the WHOLE input sink (ctx->vm->in) to EOF into one string;
// an immediately-empty stream yields "" (a bulk read cannot "fail" -> Plain return kind, no
// wrap, no prelude requirement). Zero-arg (dummy window base). Allocates one string (safepoint).
static Value native_read_all_stdin(Value*, uint8_t, Context* ctx) {
    if (ctx->vm->out) ctx->vm->out->flush();   // as readLine
    std::string all((std::istreambuf_iterator<char>(*ctx->vm->in)),
                    std::istreambuf_iterator<char>());
    GcObject* s = ctx->vm->heap->alloc_string_gc(all, ctx);
    return Value::fromPtr(s->payload());
}

// f64ToBytes(x) -> Bytes: the 8 raw IEEE-754 bytes of x, little-endian (Plain). Enables a pure-Skarn
// program -- notably the self-hosting codegen (North Star P5) -- to materialize a double literal's
// const-pool bit pattern, which the language's 48-bit Int cannot hold. The 8 bytes are read back as two
// u32 halves (each < 2^32, in-range for a 48-bit Int) -> the lo/hi Value halves the SKBC const pool
// serializes. `x` is already a normalized double (fromDouble canonicalizes -0.0/NaN), so the bytes match
// the C++ codegen's Value::fromDouble path exactly. Allocates one KIND_BYTES (safepoint).
static Value native_f64_to_bytes(Value* args, uint8_t nargs, Context* ctx) {
    double d = (nargs >= 1) ? args[0].numAsDouble() : 0.0;
    unsigned char buf[8];
    std::memcpy(buf, &d, 8);                            // host is little-endian x64 -> LE byte order
    Value result;
    bytes_from_str(ctx, &result, std::string_view(reinterpret_cast<const char*>(buf), 8));
    return result;
}

// parseInt(s) -> Int (success) | String (error message). Strict full-string base-10
// parse via std::from_chars (locale-free, non-allocating, no exceptions): the WHOLE
// string must be consumed (no leading/trailing space, no trailing junk, no leading
// '+'), and the value must fit signed 48-bit; otherwise the error String. An
// out-of-range integer is a parse FAILURE (Err), not saturated -- contrast toInt/D2I.
static Value native_parse_int(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "parseInt: invalid integer");
    std::string s;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        s.assign(o->bytes(), o->string_length());     // host copy before any alloc
    }
    const char* first = s.data();
    const char* last  = first + s.size();
    int64_t v = 0;
    auto r = std::from_chars(first, last, v);          // base 10
    constexpr int64_t MAX_48 =  140737488355327LL;     //  2^47 - 1
    constexpr int64_t MIN_48 = -140737488355328LL;     // -2^47
    if (r.ec != std::errc{} || r.ptr != last || v < MIN_48 || v > MAX_48)
        return native_make_error(ctx, "parseInt: invalid integer");
    return Value::fromSigned48(v);                      // Ok payload (immediate)
}

// parseDouble(s) -> Double (success) | String (error message). Strict full-string
// parse in chars_format::general: an optional '-', decimal digits with an optional '.'
// and an optional exponent, or one of the inf / infinity / nan spellings (any case).
// Rejected: a leading '+' or whitespace, a hexadecimal 0x form, a ',' decimal
// separator, and any trailing junk.
//
// std::from_chars is exactly that grammar, and it is locale-independent. libc++ has no
// floating-point from_chars, so the #else falls back to strtod -- which is a LOOSER
// grammar (it takes '+', leading whitespace and hex floats) and is locale-SENSITIVE via
// LC_NUMERIC. The pre-scan below removes the difference: it rejects everything strtod
// would accept and from_chars would not, so both paths accept the same strings.
static Value native_parse_double(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "parseDouble: invalid number");
    std::string s;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        s.assign(o->bytes(), o->string_length());
    }
    const char* first = s.data();
    const char* last  = first + s.size();
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double v = 0.0;
    auto r = std::from_chars(first, last, v);           // chars_format::general
    if (r.ec != std::errc{} || r.ptr != last)
        return native_make_error(ctx, "parseDouble: invalid number");
    return Value::fromDouble(v);
#else
    const auto reject = [&] { return native_make_error(ctx, "parseDouble: invalid number"); };
    if (first == last) return reject();
    // strtod SKIPS leading whitespace and then reports a clean end, so the trailing-junk
    // check below cannot see it -- it has to be rejected up front.
    if (*first == ' ' || *first == '\t' || *first == '\n' || *first == '\r'
        || *first == '\f' || *first == '\v')
        return reject();
    const char* p = first;
    if (*p == '-') ++p;                                 // '+' is NOT part of the grammar
    if (p == last) return reject();
    if (*p == '+' || *p == ' ' || *p == '\t') return reject();
    if (*p == '0' && p + 1 < last && (p[1] == 'x' || p[1] == 'X'))
        return reject();                                // no hex floats
    if (s.find(',') != std::string::npos)
        return reject();                                // no locale decimal comma
    char*  endptr = nullptr;
    double v      = std::strtod(first, &endptr);
    if (endptr != last) return reject();                // trailing junk / nothing parsed
    return Value::fromDouble(v);
#endif
}

// Drain a readable pipe to EOF into `out`. Runs on its own std::thread so
// stdout and stderr are read concurrently (a child that fills one pipe while we are
// blocked on the other would otherwise deadlock). Never throws across the thread
// boundary; a broken pipe / closed handle just ends the loop.
#ifdef _WIN32
// Quote one argument for a Win32 command line (CommandLineToArgvW inverse rules).
// CreateProcess takes ONE command line string, so build_command_line reconstructs it.
static std::string quote_win32_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
        return arg;
    std::string out = "\"";
    for (size_t i = 0; ; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++backslashes; ++i; }
        if (i == arg.size()) { out.append(backslashes * 2, '\\'); break; }
        if (arg[i] == '"') { out.append(backslashes * 2 + 1, '\\'); out.push_back('"'); }
        else { out.append(backslashes, '\\'); out.push_back(arg[i]); }
    }
    out.push_back('"');
    return out;
}
static std::string build_command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmd.push_back(' '); cmd += quote_win32_arg(argv[i]); }
    return cmd;
}
static void drain_pipe(HANDLE h, std::string* out) noexcept {
    char buf[4096]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        out->append(buf, got);
}
#else
static void drain_pipe(int fd, std::string* out) noexcept {
    char buf[4096]; ssize_t got = 0;
    while ((got = ::read(fd, buf, sizeof(buf))) > 0)
        out->append(buf, static_cast<size_t>(got));
    ::close(fd);
}
#endif

// rawRun(argv, input) -> [stdoutBytes, stderrBytes, exitInt] (success) | String (spawn error).
// The low-level process primitive; the prelude's run/runWith reshape the Array[3] into a
// ProcessOutput struct. The compiler wraps this Ok/Err (NativeReturn::Result): the success
// result is a KIND_ARRAY, so the "String => Err" discriminator routes it to Ok(array).
//   argv  -- a non-empty KIND_ARRAY of KIND_STRING; argv[0] is the program, the rest args.
//   input -- a KIND_BYTES fed to the child's stdin (then EOF), or nil for empty stdin.
// Rust Command::output semantics: only a SPAWN failure (program not found, pipe error) is an
// Err; a process that runs and exits non-zero is a success whose exitCode is data. All pipe
// I/O + threading is host-side (no VM alloc); only the final Array[3] + its two KIND_BYTES are
// allocated, with the native_args root-then-fill discipline (a per-Bytes collection can move
// the array in place).
static Value native_run_process(Value* args, uint8_t nargs, Context* ctx) {
    // --- Validate + materialize everything host-side BEFORE any VM alloc ---------------
    // argv may be a KIND_ARRAY or a KIND_VEC (the prelude's run/runWith normalize any
    // iterable to a vec first, so a bare cons-list literal works at the surface too).
    std::vector<std::string> argv;
    {
        if (!args[0].isPtr())
            return native_make_error(ctx, "run: first argument must be an array of strings");
        GcObject*     hdr = GcObject::from_slots(args[0].asPtr());
        Value*        elems = nullptr;
        uint32_t      n     = 0;
        if (hdr->kind == GcObject::KIND_ARRAY) {
            elems = hdr->slots();
            n     = static_cast<uint32_t>(hdr->slot_count());
        } else if (hdr->kind == GcObject::KIND_VEC) {
            GcObject* backing = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());
            n     = static_cast<uint32_t>(hdr->slots()[VEC_SLOT_COUNT].asSigned48());
            elems = backing->slots();
        } else {
            return native_make_error(ctx, "run: first argument must be an array of strings");
        }
        argv.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (!is_string(elems[i]))
                return native_make_error(ctx, "run: argv elements must be strings");
            GcObject* s = GcObject::from_slots(elems[i].asPtr());
            argv.emplace_back(s->bytes(), s->string_length());
        }
    }
    if (argv.empty())
        return native_make_error(ctx, "run: argv must not be empty");
    std::string stdin_data;                         // empty unless a KIND_BYTES input is given
    if (nargs >= 2 && args[1].isPtr() &&
        GcObject::from_slots(args[1].asPtr())->kind == GcObject::KIND_BYTES) {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        stdin_data.assign(backing->bytes(), count);
    }

    // --- Spawn child process with three pipes (stdin/stdout/stderr) ----------------
    std::string out_buf, err_buf;
    int64_t exit_code = 0;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE in_rd = nullptr, in_wr = nullptr;
    HANDLE out_rd = nullptr, out_wr = nullptr;
    HANDLE err_rd = nullptr, err_wr = nullptr;
    auto close_all = [&]() {
        for (HANDLE* h : { &in_rd, &in_wr, &out_rd, &out_wr, &err_rd, &err_wr })
            if (*h) { CloseHandle(*h); *h = nullptr; }
    };
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0) ||
        !CreatePipe(&out_rd, &out_wr, &sa, 0) ||
        !CreatePipe(&err_rd, &err_wr, &sa, 0)) {
        close_all();
        return native_make_error(ctx, "run: could not create pipes for " + argv[0]);
    }
    SetHandleInformation(in_wr,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd; si.hStdOutput = out_wr; si.hStdError = err_wr;
    PROCESS_INFORMATION pi{};
    std::string cmdline = build_command_line(argv);
    std::vector<char> cmd_mut(cmdline.begin(), cmdline.end()); cmd_mut.push_back('\0');
    if (!CreateProcessA(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        DWORD gle = GetLastError(); close_all();
        return native_make_error(ctx, "run: could not start process: " + argv[0] +
                                      " (error " + std::to_string(gle) + ")");
    }
    CloseHandle(in_rd); CloseHandle(out_wr); CloseHandle(err_wr);
    in_rd = out_wr = err_wr = nullptr;
    std::thread t_out(drain_pipe, out_rd, &out_buf);
    std::thread t_err(drain_pipe, err_rd, &err_buf);
    if (!stdin_data.empty()) {
        const char* p = stdin_data.data(); size_t left = stdin_data.size();
        while (left > 0) {
            DWORD wrote = 0; DWORD chunk = static_cast<DWORD>(left > (1u<<20) ? (1u<<20) : left);
            if (!WriteFile(in_wr, p, chunk, &wrote, nullptr) || wrote == 0) break;
            p += wrote; left -= wrote;
        }
    }
    CloseHandle(in_wr); in_wr = nullptr;
    t_out.join(); t_err.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD wec = 0; GetExitCodeProcess(pi.hProcess, &wec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(out_rd); CloseHandle(err_rd);
    exit_code = static_cast<int64_t>(static_cast<int32_t>(wec));
#else
    int in_pipe[2]  = {-1, -1};   // [read-end, write-end]: child reads, parent writes
    int out_pipe[2] = {-1, -1};   // [read-end, write-end]: parent reads, child writes
    int err_pipe[2] = {-1, -1};
    // exec_err_pipe: O_CLOEXEC write-end is closed by exec on success; child writes errno on
    // exec failure so the parent can distinguish "exec failed" from "process exited non-zero".
    int exec_err_pipe[2] = {-1, -1};
    auto close_fds = [&]() {
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                       err_pipe[0], err_pipe[1], exec_err_pipe[0], exec_err_pipe[1]})
            if (fd >= 0) ::close(fd);
    };
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0 ||
        pipe(exec_err_pipe) != 0) {
        close_fds();
        return native_make_error(ctx, "run: could not create pipes for " + argv[0]);
    }
    fcntl(exec_err_pipe[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close_fds();
        return native_make_error(ctx, "run: fork failed for " + argv[0] + ": " + std::strerror(errno));
    }
    if (pid == 0) {
        // Child: wire up stdio then exec
        ::close(exec_err_pipe[0]);
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
            ::close(fd);
        std::vector<char*> exec_argv;
        exec_argv.reserve(argv.size() + 1);
        for (auto& s : argv) exec_argv.push_back(const_cast<char*>(s.c_str()));
        exec_argv.push_back(nullptr);
        execvp(exec_argv[0], exec_argv.data());
        // exec failed: send errno back to parent then exit
        int child_errno = errno;
        ::write(exec_err_pipe[1], &child_errno, sizeof(child_errno));
        _exit(127);
    }
    // Parent: close child-side ends so EOF propagates
    ::close(in_pipe[0]);  in_pipe[0]  = -1;
    ::close(out_pipe[1]); out_pipe[1] = -1;
    ::close(err_pipe[1]); err_pipe[1] = -1;
    // Read from exec_err_pipe: returns sizeof(int) bytes if exec failed (errno), 0 bytes
    // (EOF via O_CLOEXEC) if exec succeeded.
    ::close(exec_err_pipe[1]); exec_err_pipe[1] = -1;
    int child_exec_errno = 0;
    ssize_t exec_err_n;
    do { exec_err_n = ::read(exec_err_pipe[0], &child_exec_errno, sizeof(child_exec_errno)); }
    while (exec_err_n < 0 && errno == EINTR);
    ::close(exec_err_pipe[0]); exec_err_pipe[0] = -1;
    if (exec_err_n == static_cast<ssize_t>(sizeof(child_exec_errno))) {
        waitpid(pid, nullptr, 0);
        ::close(in_pipe[1]); in_pipe[1] = -1;
        // drain threads haven't started yet so drain fds manually
        ::close(out_pipe[0]); out_pipe[0] = -1;
        ::close(err_pipe[0]); err_pipe[0] = -1;
        return native_make_error(ctx, "run: could not start process: " + argv[0] +
                                      ": " + std::strerror(child_exec_errno));
    }
    std::thread t_out(drain_pipe, out_pipe[0], &out_buf);  // drain_pipe closes fd on return
    std::thread t_err(drain_pipe, err_pipe[0], &err_buf);
    out_pipe[0] = err_pipe[0] = -1;  // drain_pipe owns/closes these
    if (!stdin_data.empty()) {
        const char* p = stdin_data.data(); size_t left = stdin_data.size();
        while (left > 0) {
            ssize_t wrote = ::write(in_pipe[1], p, left);
            if (wrote <= 0) break;
            p += wrote; left -= static_cast<size_t>(wrote);
        }
    }
    ::close(in_pipe[1]); in_pipe[1] = -1;   // EOF to child's stdin
    t_out.join(); t_err.join();
    int wstatus = 0; waitpid(pid, &wstatus, 0);
    exit_code = static_cast<int64_t>(WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1);
#endif

    // --- Build the Array[3] result (root-then-fill; see native_args) --------------------
    Heap* heap = ctx->vm->heap;
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, 3, ctx);
    Value arr = Value::fromPtr(arrObj->payload());
    heap->add_root(&arr);                           // the two Bytes allocs below can move it
    Value out_bytes;
    bytes_from_str(ctx, &out_bytes, out_buf);       // may collect -> arr rewritten in place
    GcObject::from_slots(arr.asPtr())->slots()[0] = out_bytes;
    Value err_bytes;
    bytes_from_str(ctx, &err_bytes, err_buf);       // may collect -> arr rewritten in place
    GcObject::from_slots(arr.asPtr())->slots()[1] = err_bytes;
    GcObject::from_slots(arr.asPtr())->slots()[2] = Value::fromSigned48(exit_code);
    heap->remove_root(&arr);
    return arr;
}

// rawGcStats() -> Array[Double] (Plain -- no wrap). A runtime GC introspection hook: the 8
// Heap::GcStats counters, in the field order of the Skarn `GcStats` struct that the prelude
// `gcStats()` wrapper unpacks. Each counter is a uint64; a DOUBLE (53-bit exact-integer range)
// carries it faithfully for any realistic run -- unlike a 48-bit Int, so this sidesteps the
// int-range caveat entirely. Non-deterministic (depends on heap history), so it is EXCLUDED from
// the differential (RefEval leaves it Unsupported, like nanoTime). Builds a fresh KIND_ARRAY of
// 8 immediate doubles; the doubles are immediates (no per-element heap object), so no root-while-
// filling is needed -- the single array alloc is the only safepoint, and stats() is read after it.
static Value native_raw_gc_stats(Value*, uint8_t, Context* ctx) {
    Heap* heap = ctx->vm->heap;
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, 8, ctx);
    const Heap::GcStats& s = heap->stats();   // read AFTER the alloc (no move afterwards)
    Value* slot = arrObj->slots();
    slot[0] = Value::fromDouble(static_cast<double>(s.collections));
    slot[1] = Value::fromDouble(static_cast<double>(s.objects_alloced));
    slot[2] = Value::fromDouble(static_cast<double>(s.bytes_alloced));
    slot[3] = Value::fromDouble(static_cast<double>(s.from_used_sum));
    slot[4] = Value::fromDouble(static_cast<double>(s.survivors_sum));
    slot[5] = Value::fromDouble(static_cast<double>(s.gc_ns_total));
    slot[6] = Value::fromDouble(static_cast<double>(s.gc_ns_max));
    slot[7] = Value::fromDouble(static_cast<double>(s.grow_events));
    return Value::fromPtr(arrObj->payload());
}

// gcResetStats() -> nil (Plain). Clears the GcStats counters so a subsequent gcStats() measures
// GC over a code region (reset -> ...code... -> gcStats()). Non-allocating, non-deterministic.
static Value native_gc_reset_stats(Value*, uint8_t, Context* ctx) {
    ctx->vm->heap->reset_stats();
    return Value::fromNil();
}

// =============================================================================
// TCP networking natives (std::net). Blocking sockets over Winsock on Windows and
// over the BSD socket API elsewhere (Platform.h hides the difference). A socket is
// exposed to Skarn as a small Int DESCRIPTOR -- an index into this per-execution
// registry (VM::net) -- never a raw OS SOCKET (a 64-bit kernel handle that need
// not fit a 48-bit Int). The registry closes any still-open socket at execute()
// teardown (RAII), the safety net behind explicit tcpClose. NOT a GC root (it
// holds integer handles, no Values). Each native returns a Bytes/Int/nil on
// success or a String error message (the compiler wraps Ok/Err, NRET_RESULT). The NetRegistry type
// itself is defined near the top of this file (execute() holds a stack-local instance).
// =============================================================================

// Lazy, once-only WSAStartup. The latch is a MAGIC STATIC, not a plain `bool` pair: initialization
// of a function-local static is exactly-once and thread-safe by the language rule (MSVC implements
// it under /Zc:threadSafeInit, which is on by default), so several VM instances on several threads
// may call this concurrently. The hand-rolled `inited`/`ok` pair it replaces was a data race -- the
// unsynchronized read of `ok` could return false while another thread was still inside WSAStartup,
// turning a healthy socket call into a bogus "WSAStartup failed". No WSACleanup: process exit
// reclaims the Winsock state, and a paired cleanup would race a still-open socket.
static bool ensure_wsa() {
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
#else
    return true;  // POSIX sockets need no initialization
#endif
}

static std::string net_error_msg(const char* op) {
#ifdef _WIN32
    return std::string(op) + " failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
#else
    return std::string(op) + " failed: " + std::strerror(errno);
#endif
}

// Switch a socket between blocking and non-blocking mode; 0 on success, -1 on failure. Both
// platforms in one place: the timed connect below uses it to bound a dead host, and the std::poll
// natives use it for their whole purpose -- a socket that answers "would block" instead of waiting.
static int set_nonblocking(socket_t s, bool nb) {
#ifdef _WIN32
    u_long v = nb ? 1u : 0u;
    return (ioctlsocket(s, FIONBIO, &v) == 0) ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}
static const long NET_CONNECT_TIMEOUT_SEC = 10;   // connect timeout (select() on POSIX, TCP_MAXRT on Windows)
static const long NET_RECV_CHUNK_MAX      = 1 << 20; // cap a single tcpRecv at 1 MiB

// tcpConnect(host, port) -> Int descriptor (success) | String (error). Resolves host with
// getaddrinfo(AF_UNSPEC) so BOTH IPv4 and IPv6 addresses are tried; connect is bounded by
// NET_CONNECT_TIMEOUT_SEC so a dead host does not hang the VM.
static Value native_tcp_connect(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpConnect: networking unavailable");
    if (nargs < 2 || !is_string(args[0]) || !args[1].isInt())
        return native_make_error(ctx, "tcpConnect: expected (host: String, port: Int)");
    std::string host;
    { GcObject* o = GcObject::from_slots(args[0].asPtr()); host.assign(o->bytes(), o->string_length()); }
    long port = static_cast<long>(args[1].asSigned48());
    if (port < 0 || port > 65535) return native_make_error(ctx, "tcpConnect: port out of range (0..65535)");
    if (!ensure_wsa()) return native_make_error(ctx, "tcpConnect: WSAStartup failed");

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return native_make_error(ctx, "tcpConnect: cannot resolve host: " + host);

    socket_t sock = INVALID_SOCK;
    for (addrinfo* ai = res; ai && sock == INVALID_SOCK; ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) continue;
        sock_suppress_sigpipe(s);
#ifdef _WIN32
        // Windows: a BLOCKING connect, bounded by TCP_MAXRT (the SYN retransmission limit, in seconds).
        // The non-blocking connect + select() below works here too, but on Windows it intermittently
        // waits one full timer tick (~15.6 ms) even on loopback, where a blocking connect returns in
        // ~0.1 ms (WSAPoll behaves like select). A load generator would report that tick as server
        // latency.
        DWORD maxrt = static_cast<DWORD>(NET_CONNECT_TIMEOUT_SEC);
        setsockopt(s, IPPROTO_TCP, TCP_MAXRT, reinterpret_cast<const char*>(&maxrt), sizeof(maxrt));
        const bool ok = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0;
#else
        set_nonblocking(s, true);
        int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = (rc == 0);
        if (rc == -1 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
            fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
            timeval tv{}; tv.tv_sec = NET_CONNECT_TIMEOUT_SEC; tv.tv_usec = 0;
            if (select(s + 1, nullptr, &wf, nullptr, &tv) > 0) {
                int soErr = 0; socklen_t len = sizeof(soErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
                ok = (soErr == 0);
            }
        }
        set_nonblocking(s, false);   // every descriptor Skarn sees from std::net is blocking
#endif
        if (ok) sock = s; else sock_close(s);
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCK) return native_make_error(ctx, "tcpConnect: could not connect to " + host);
    return Value::fromSigned48(ctx->vm->net->add(sock));
}

// tcpSend(sock, data) -> nil (success) | String (error). Sends the buffer's live bytes in full
// (loops until every byte is flushed or an error occurs).
static Value native_tcp_send(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpSend: networking unavailable");
    if (nargs < 2 || !args[0].isInt())
        return native_make_error(ctx, "tcpSend: expected (sock: Int, data: Bytes)");
    if (!args[1].isPtr() || GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "tcpSend: data must be a byte buffer");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("tcpSend: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);   // host copy (no alloc happens after this before the sends)
    }
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), SOCK_SEND_FLAGS);
        if (n < 0) return native_make_error(ctx, "tcpSend: " + net_error_msg("send"));
        sent += static_cast<size_t>(n);
    }
    return Value::fromNil();
}

// tcpRecv(sock, maxBytes) -> Bytes (success; EMPTY = EOF) | String (error). One recv of up to
// maxBytes; a 0-length return is an orderly peer shutdown, surfaced as an empty Bytes.
static Value native_tcp_recv(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpRecv: networking unavailable");
    if (nargs < 2 || !args[0].isInt() || !args[1].isInt())
        return native_make_error(ctx, "tcpRecv: expected (sock: Int, maxBytes: Int)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("tcpRecv: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    long maxB = static_cast<long>(args[1].asSigned48());
    if (maxB < 0) return native_make_error(ctx, "tcpRecv: maxBytes must be non-negative");
    if (maxB > NET_RECV_CHUNK_MAX) maxB = NET_RECV_CHUNK_MAX;
    std::string buf;
    buf.resize(static_cast<size_t>(maxB));
    int n = (maxB == 0) ? 0 : ::recv(s, buf.data(), static_cast<int>(maxB), 0);
    if (n < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAETIMEDOUT) return native_make_error(ctx, "tcpRecv: timeout");
#else
        if (errno == EAGAIN || errno == ETIMEDOUT) return native_make_error(ctx, "tcpRecv: timeout");
#endif
        return native_make_error(ctx, "tcpRecv: " + net_error_msg("recv"));
    }
    const std::string got(buf.data(), static_cast<size_t>(n));  // host copy before the allocating build
    Value result;
    bytes_from_str(ctx, &result, got);
    return result;
}

// tcpClose(sock) -> nil (success) | String (error). Closes the socket and frees its registry slot.
// Used for a client connection AND for a server's listen socket (self-shutdown).
static Value native_tcp_close(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpClose: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpClose: expected (sock: Int)");
    const int64_t fd = args[0].asSigned48();
    socket_t s = ctx->vm->net->get(fd);
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("tcpClose: ") + ctx->vm->net->invalid_reason(fd));
    sock_close(s);
    ctx->vm->net->drop(fd);
    return Value::fromNil();
}

// tcpListen(port) -> Int listen-descriptor (success) | String (error). Creates a DUAL-STACK IPv6
// listener (IPV6_V6ONLY=0), so it accepts both IPv6 and IPv4-mapped clients (a "127.0.0.1" connect works).
static Value native_tcp_listen(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpListen: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpListen: expected (port: Int)");
    long port = static_cast<long>(args[0].asSigned48());
    if (port < 0 || port > 65535) return native_make_error(ctx, "tcpListen: port out of range (0..65535)");
    if (!ensure_wsa()) return native_make_error(ctx, "tcpListen: WSAStartup failed");
    socket_t s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpListen: " + net_error_msg("socket"));
    sock_suppress_sigpipe(s);
#ifdef _WIN32
    DWORD v6only = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), sizeof(v6only));
    BOOL  reuse  = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
#else
    int v6only = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    int reuse  = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6; addr.sin6_addr = in6addr_any; addr.sin6_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string m = "tcpListen: " + net_error_msg("bind"); sock_close(s); return native_make_error(ctx, m);
    }
    if (listen(s, SOMAXCONN) < 0) {
        std::string m = "tcpListen: " + net_error_msg("listen"); sock_close(s); return native_make_error(ctx, m);
    }
    return Value::fromSigned48(ctx->vm->net->add(s));
}

// tcpAccept(listenSock) -> Int descriptor of an accepted connection (success) | String (error).
// BLOCKS until a client connects (the single-threaded VM serves one connection at a time).
static Value native_tcp_accept(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpAccept: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpAccept: expected (sock: Int)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("tcpAccept: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    socket_t c = accept(s, nullptr, nullptr);
    if (c == INVALID_SOCK) return native_make_error(ctx, "tcpAccept: " + net_error_msg("accept"));
    sock_suppress_sigpipe(c);
    return Value::fromSigned48(ctx->vm->net->add(c));
}

// tcpLocalPort(sock) -> Int port (success) | String (error). The port the socket is actually bound
// to, via getsockname. Its reason to exist is `tcpListen(0)`: asking the OS for a free port is the
// only way to run a server without guessing a fixed number, and on Windows the dynamic range is
// 1024-60000, so ANY hardcoded port can collide with an unrelated outbound connection (an observed,
// intermittent `bind` WSAEACCES). Reads the v6 sockaddr the dual-stack listener binds, but falls
// back to the v4 layout so it is also correct for a `tcpConnect` descriptor.
static Value native_tcp_local_port(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpLocalPort: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpLocalPort: expected (sock: Int)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("tcpLocalPort: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    sockaddr_storage ss{};
    socklen_t len = static_cast<socklen_t>(sizeof(ss));
    if (getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) < 0)
        return native_make_error(ctx, "tcpLocalPort: " + net_error_msg("getsockname"));
    uint16_t net_port = 0;
    if (ss.ss_family == AF_INET6)      net_port = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port;
    else if (ss.ss_family == AF_INET)  net_port = reinterpret_cast<sockaddr_in*>(&ss)->sin_port;
    else return native_make_error(ctx, "tcpLocalPort: unsupported address family");
    return Value::fromSigned48(static_cast<int64_t>(ntohs(net_port)));
}

// tcpSetTimeout(sock, ms) -> nil (success) | String (error). Sets the recv/send timeout in
// milliseconds; ms <= 0 means block indefinitely (the default for a fresh socket).
static Value native_tcp_set_timeout(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpSetTimeout: networking unavailable");
    if (nargs < 2 || !args[0].isInt() || !args[1].isInt())
        return native_make_error(ctx, "tcpSetTimeout: expected (sock: Int, ms: Int)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("tcpSetTimeout: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    long ms = static_cast<long>(args[1].asSigned48());
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms < 0 ? 0 : ms);   // Windows SO_*TIMEO: DWORD of milliseconds
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
#else
    struct timeval tv{};
    if (ms > 0) { tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    return Value::fromNil();
}

// =============================================================================
// Non-blocking I/O + readiness polling (std::poll). The natives behind the prelude's event loop:
// a socket switched to non-blocking mode answers "would block" instead of waiting, and rawPoll
// reports which of a set of sockets can be acted on. Everything else -- the loop, the per-connection
// state, the unsent-tail bookkeeping -- is written in Skarn on top of these five.
//
// The "would block" outcome is neither a value nor an error, so each native encodes it in the
// SUCCESS channel and the prelude turns it into an enum arm: -1 for accept, a 0-element array for
// recv, a 0 count for send. It must never be folded into an empty read, because an empty read
// already means the peer closed -- the exact conflation this module exists to remove.
// =============================================================================

// Skarn-side readiness flags, mirrored by the `READABLE` / `WRITABLE` / `CLOSED` consts in
// std/poll.skn. They are a packed Int rather than an enum because a socket can be several of these
// at once, which is precisely what an enum cannot say.
static constexpr int64_t SK_POLL_READABLE = 1;
static constexpr int64_t SK_POLL_WRITABLE = 2;
static constexpr int64_t SK_POLL_CLOSED   = 4;

// Read a Vec[Int] or Array[Int] argument into host memory. Same shape as rawRun's argv walk: the
// whole read happens BEFORE any allocation, so nothing here can be moved out from under us.
static bool read_int_seq(Value v, std::vector<int64_t>* out) {
    if (!v.isPtr()) return false;
    GcObject* hdr   = GcObject::from_slots(v.asPtr());
    Value*    elems = nullptr;
    uint32_t  n     = 0;
    if (hdr->kind == GcObject::KIND_ARRAY) {
        elems = hdr->slots();
        n     = static_cast<uint32_t>(hdr->slot_count());
    } else if (hdr->kind == GcObject::KIND_VEC) {
        GcObject* backing = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());
        n     = static_cast<uint32_t>(hdr->slots()[VEC_SLOT_COUNT].asSigned48());
        elems = backing->slots();
    } else {
        return false;
    }
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!elems[i].isInt()) return false;
        out->push_back(elems[i].asSigned48());
    }
    return true;
}

// True when the last socket call failed only because it would have blocked -- the one "error" that
// is not one. Windows reports it through WSAGetLastError, POSIX through errno, and POSIX is allowed
// to use either of two spellings that may or may not be the same value.
static bool sock_would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// rawSetNonBlocking(sock, on) -> nil (success) | String (error). Switches an existing descriptor
// between the two modes. std::net hands out blocking sockets only, so this is what the std::poll
// wrappers call to take one over.
static Value native_raw_set_non_blocking(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "rawSetNonBlocking: networking unavailable");
    if (nargs < 2 || !args[0].isInt() || !args[1].isBool())
        return native_make_error(ctx, "rawSetNonBlocking: expected (sock: Int, on: Bool)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("rawSetNonBlocking: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    if (set_nonblocking(s, args[1].asBool()) != 0)
        return native_make_error(ctx, "rawSetNonBlocking: " + net_error_msg("fcntl/ioctlsocket"));
    return Value::fromNil();
}

// rawPoll(fds, interest, timeoutMs) -> Array[Int] (success) | String (error). Waits until at least
// one socket is ready or the timeout expires, and answers INDEX-PARALLEL to `fds`: element i holds
// the ready flags for fds[i], 0 when nothing happened. `interest` is the per-socket mask of what the
// caller cares about -- a server with nothing to write must be able NOT to ask for writability, or
// poll returns immediately every time and the loop spins.
//
// Allocation: exactly one, for the result array, and its elements are immediates -- so nothing can
// move after the array exists and no rooting is needed (the native_raw_gc_stats shape, not the
// native_args one).
static Value native_raw_poll(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "rawPoll: networking unavailable");
    if (nargs < 3 || !args[2].isInt())
        return native_make_error(ctx, "rawPoll: expected (fds: Vec[Int], interest: Vec[Int], timeoutMs: Int)");
    std::vector<int64_t> fds, interest;
    if (!read_int_seq(args[0], &fds) || !read_int_seq(args[1], &interest))
        return native_make_error(ctx, "rawPoll: fds and interest must be sequences of Int");
    if (fds.size() != interest.size())
        return native_make_error(ctx, "rawPoll: fds and interest must have the same length");

    std::vector<pollfd_t> pfds;
    pfds.reserve(fds.size());
    bool any_interest = false;
    for (size_t i = 0; i < fds.size(); ++i) {
        socket_t s = ctx->vm->net->get(fds[i]);
        if (s == INVALID_SOCK) return native_make_error(ctx, std::string("rawPoll: ") + ctx->vm->net->invalid_reason(fds[i]));
        pollfd_t p{};
        p.fd     = s;
        p.events = static_cast<short>(((interest[i] & SK_POLL_READABLE) ? POLL_READ  : 0) |
                                      ((interest[i] & SK_POLL_WRITABLE) ? POLL_WRITE : 0));
        if (p.events != 0) any_interest = true;
        pfds.push_back(p);
    }

    // A platform difference that would otherwise surface as a baffling error: POSIX poll() accepts an
    // all-zero events set -- and SLEEPS out the timeout, which is what makes poll(NULL, 0, ms) the
    // canonical portable sleep -- while WSAPoll REJECTS it with WSAEINVAL. Asking for nothing is a
    // legitimate state for a loop with nothing outstanding this round.
    //
    // So the set is levelled by EMULATING what POSIX does, not by adopting what Winsock can express:
    // wait out the timeout, then report nothing ready. Returning early here instead would turn such a
    // round into a 100 % CPU spin -- on POSIX that would be a REGRESSION, since its own poll() got
    // this right. Do not "simplify" this branch back into an immediate return.
    const long timeout_ms = static_cast<long>(args[2].asSigned48());
    if (!pfds.empty() && any_interest) {
        const int rc = sock_poll(pfds.data(), static_cast<unsigned>(pfds.size()),
                                 static_cast<int>(timeout_ms));
        if (rc < 0) return native_make_error(ctx, "rawPoll: " + net_error_msg("poll"));
    } else {
        // Nothing to watch. A negative timeout means "wait indefinitely", and with no interest at all
        // nothing could ever end that wait -- a guaranteed hang, so it is an error rather than a VM
        // that freezes indistinguishably from a crash. (A deliberate departure from poll(NULL, 0, -1).)
        if (timeout_ms < 0)
            return native_make_error(ctx, "rawPoll: a negative timeout with no interest would wait forever");
        if (timeout_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        for (pollfd_t& p : pfds) p.revents = 0;
    }

    const uint32_t n = static_cast<uint32_t>(pfds.size());
    GcObject* arrObj = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY, n, ctx);
    Value     arr    = Value::fromPtr(arrObj->payload());
    Value*    slots  = GcObject::from_slots(arr.asPtr())->slots();
    for (uint32_t i = 0; i < n; ++i) {
        const short re = pfds[i].revents;
        int64_t flags = 0;
        if (re & POLL_READ)                            flags |= SK_POLL_READABLE;
        if (re & POLL_WRITE)                           flags |= SK_POLL_WRITABLE;
        if (re & (POLLERR | POLLHUP | POLLNVAL))       flags |= SK_POLL_CLOSED;
        slots[i] = Value::fromSigned48(flags);   // immediates only -> no further allocation, no root
    }
    return arr;
}

// rawAcceptNb(listenSock) -> Int (success) | String (error). >= 0 is the descriptor of an accepted
// connection, -1 means "would block" -- nobody is waiting. The accepted socket is explicitly put
// into non-blocking mode: whether it INHERITS the listener's mode differs between platforms
// (Windows inherits, BSD/macOS does not), and the loop must not depend on which one it is on.
static Value native_raw_accept_nb(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "rawAcceptNb: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "rawAcceptNb: expected (sock: Int)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("rawAcceptNb: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    socket_t c = accept(s, nullptr, nullptr);
    if (c == INVALID_SOCK) {
        if (sock_would_block()) return Value::fromSigned48(-1);
        return native_make_error(ctx, "rawAcceptNb: " + net_error_msg("accept"));
    }
    sock_suppress_sigpipe(c);
    set_nonblocking(c, true);
    return Value::fromSigned48(ctx->vm->net->add(c));
}

// rawRecvNb(sock, maxBytes) -> Array[Bytes] (success) | String (error). THREE outcomes in one call:
// an EMPTY array means "would block", a 1-element array holds the read, and that element being
// empty means the peer closed. The array is the carrier because Bytes alone cannot say three
// things -- empty is already taken by EOF -- and a homogeneous Array[Bytes] types without a codegen
// special case, unlike rawRun's heterogeneous Array[3]. The prelude hides it behind `Received`.
static Value native_raw_recv_nb(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "rawRecvNb: networking unavailable");
    if (nargs < 2 || !args[0].isInt() || !args[1].isInt())
        return native_make_error(ctx, "rawRecvNb: expected (sock: Int, maxBytes: Int)");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("rawRecvNb: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    long maxB = static_cast<long>(args[1].asSigned48());
    if (maxB < 0) return native_make_error(ctx, "rawRecvNb: maxBytes must be non-negative");
    if (maxB > NET_RECV_CHUNK_MAX) maxB = NET_RECV_CHUNK_MAX;

    std::string buf;
    buf.resize(static_cast<size_t>(maxB));
    const int n = (maxB == 0) ? 0 : ::recv(s, buf.data(), static_cast<int>(maxB), 0);
    Heap* heap = ctx->vm->heap;
    if (n < 0) {
        if (sock_would_block())                       // the empty array: nothing to read YET
            return Value::fromPtr(heap->alloc_slots_gc(GcObject::KIND_ARRAY, 0, ctx)->payload());
        return native_make_error(ctx, "rawRecvNb: " + net_error_msg("recv"));
    }
    const std::string got(buf.data(), static_cast<size_t>(n));  // host copy before the allocating build

    // Build the payload FIRST and keep it rooted across the array allocation: bytes_from_str
    // allocates twice internally, and the array allocation below can move what it produced.
    Value payload = Value::fromNil();
    heap->add_root(&payload);
    bytes_from_str(ctx, &payload, got);
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, 1, ctx);   // may collect -> payload moves
    Value     arr    = Value::fromPtr(arrObj->payload());
    GcObject::from_slots(arr.asPtr())->slots()[0] = payload;                 // re-read through the root
    heap->remove_root(&payload);
    return arr;
}

// rawSendNb(sock, data) -> Int (success) | String (error). The count of bytes the kernel ACCEPTED,
// which may be less than what was offered: on a non-blocking socket a full send buffer is normal,
// not an error. 0 means nothing went out (would block). There is deliberately no send-all loop --
// it cannot exist here; the caller keeps the unsent tail and tries again when poll says writable.
static Value native_raw_send_nb(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "rawSendNb: networking unavailable");
    if (nargs < 2 || !args[0].isInt())
        return native_make_error(ctx, "rawSendNb: expected (sock: Int, data: Bytes)");
    if (!args[1].isPtr() || GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "rawSendNb: data must be a byte buffer");
    socket_t s = ctx->vm->net->get(args[0].asSigned48());
    if (s == INVALID_SOCK) return native_make_error(ctx, std::string("rawSendNb: ") + ctx->vm->net->invalid_reason(args[0].asSigned48()));
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);   // host copy; nothing allocates before the send
    }
    if (data.empty()) return Value::fromSigned48(0);
    const int n = ::send(s, data.data(), static_cast<int>(data.size()), SOCK_SEND_FLAGS);
    if (n < 0) {
        if (sock_would_block()) return Value::fromSigned48(0);
        return native_make_error(ctx, "rawSendNb: " + net_error_msg("send"));
    }
    return Value::fromSigned48(static_cast<int64_t>(n));
}

// =============================================================================
// SHA-256 (FIPS 180-4). A native because 32-bit modular arithmetic is awkward in a 48-bit-Int
// language; pure + deterministic, so KAT-anchored (empty / "abc" / the fox vector). Returns the raw
// 32-byte digest as Bytes; the std::hash prelude adds the hex-string helpers on top.
// =============================================================================
static void sha256_digest(const uint8_t* msg, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    // Padding: append 0x80, then zeros up to 56 mod 64, then the 64-bit big-endian bit length.
    std::vector<uint8_t> m(msg, msg + len);
    const uint64_t bitlen = static_cast<uint64_t>(len) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0x00);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bitlen >> (i * 8)));
    auto rotr = [](uint32_t x, int n) -> uint32_t { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(m[off + 4*i])     << 24) | (static_cast<uint32_t>(m[off + 4*i + 1]) << 16) |
                   (static_cast<uint32_t>(m[off + 4*i + 2]) <<  8) |  static_cast<uint32_t>(m[off + 4*i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            const uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; ++i) {
        out[4*i]   = static_cast<uint8_t>(h[i] >> 24);
        out[4*i+1] = static_cast<uint8_t>(h[i] >> 16);
        out[4*i+2] = static_cast<uint8_t>(h[i] >> 8);
        out[4*i+3] = static_cast<uint8_t>(h[i]);
    }
}

// sha256(data: Bytes) -> Bytes (the 32-byte digest; Plain, always succeeds). The checker guarantees a
// Bytes argument; a non-Bytes (only reachable via hand-assembled bytecode) hashes an empty input.
static Value native_sha256(Value* args, uint8_t nargs, Context* ctx) {
    std::string data;
    if (nargs >= 1 && args[0].isPtr() &&
        GcObject::from_slots(args[0].asPtr())->kind == GcObject::KIND_BYTES) {
        GcObject*      hdr     = GcObject::from_slots(args[0].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);   // host copy before the allocating build below
    }
    uint8_t dg[32];
    sha256_digest(reinterpret_cast<const uint8_t*>(data.data()), data.size(), dg);
    Value result;
    bytes_from_str(ctx, &result, std::string(reinterpret_cast<const char*>(dg), 32));
    return result;
}

// =============================================================================
// Isolate natives -- tasks (rawSpawn / rawJoin / rawTaskTake), actors (rawSpawnActor / rawSend /
// rawReceive / rawMailMsg / rawMailFrom / rawMailReason / rawMainInbox), their extra and bounded
// inboxes (rawNewInbox / rawCloseInbox / rawTrySend / rawSpawnActorBounded), and for both
// rawTaskInput and rawSelfId. The runtime they share (World, Isolate, Mailbox) is described above
// execute().
//
// HOW AN ISOLATE STARTS AT A FUNCTION. execute() normally begins at instruction 0, the top-level
// code. An isolate instead gets the world's code with a short ENTRY STUB appended -- appending
// keeps every function's code_offset valid -- and starts there (TaskEntry::entry_pc). The stub is
// ordinary bytecode, so the call goes through the VM's own calling convention:
//     r1 = NATIVE_TASK_INPUT ; r2 = rawTaskInput()    -- the argument, decoded in the NEW heap
//     r0 = Func(fn)          ; CALL_INDIRECT r0, 1    -- fn(r2); the result comes back in r2
//     MOV_TAKE r0, r2        ; HALT
// Top frame size 2: the argument sits at r2 = the outgoing window, exactly where CALL_INDIRECT
// wants it. Skarn has no global variables, so skipping the top-level code loses nothing a top-level
// function could depend on. An actor uses the same one-argument stub: std::actor starts a
// trampoline that takes `{ f, init }` and asks rawSelfId for the actor's own id.
// =============================================================================
static constexpr uint8_t TASK_STUB_FRAME = 2;

// An actor starts with a SMALL heap (the heap grows when it must): a task is one computation, but
// a program may run hundreds of actors, and the default 2 x 4 MiB each would add up to gigabytes.
static constexpr size_t ACTOR_INITIAL_SEMI = 64 * 1024;

static std::vector<uint32_t> task_code(const std::vector<uint32_t>& program, uint16_t fn_id) {
    auto op = [](OpCode o) { return static_cast<uint8_t>(o); };
    std::vector<uint32_t> code;
    code.reserve(program.size() + 6);
    code = program;
    code.push_back(Instruction::C2(op(OpCode::LOAD_CONST), 1, static_cast<int16_t>(NATIVE_TASK_INPUT)).raw);
    code.push_back(Instruction::CallNative(op(OpCode::CALL_NATIVE), 2, 1, 2, 0).raw);
    code.push_back(Instruction::C2(op(OpCode::LOAD_FN), 0, static_cast<int16_t>(fn_id)).raw);
    code.push_back(Instruction::R6(op(OpCode::CALL_INDIRECT), 0, 0, 1).raw);
    code.push_back(Instruction::R6(op(OpCode::MOV_TAKE), 0, 2, 0).raw);
    code.push_back(Instruction::J(op(OpCode::HALT)).raw);
    return code;
}

// The body of an isolate's thread. Catches EVERYTHING: an exception escaping a std::thread
// terminates the process.
//   * A TASK buffers its output and hands it over at join; on success its result is encoded
//     straight after execute() returns (encode() allocates nothing, so the heap cannot collect
//     between HALT and the copy).
//   * An ACTOR writes its output a line at a time to the root's stream. When it ends all its inboxes
//     are closed FIRST, so a sender told about the crash can no longer reach it (and a sender blocked
//     on a full inbox of it is let go); then, if it FAULTED, its starter gets MAIL_EXITED (v1 reports
//     crashes only).
static void run_isolate(World* world, std::shared_ptr<Isolate> iso, uint16_t fn_id) {
    std::ostringstream task_out;
    LineForwardBuf     actor_buf(world, /*always_lines=*/true);
    std::ostream       actor_out(&actor_buf);
    std::ostream&      out = iso->actor ? actor_out : static_cast<std::ostream&>(task_out);
    std::string        fault;
    bool               faulted = false;
    try {
        const ProgramImage* img  = world->image;
        const std::vector<uint32_t> code = task_code(*img->bytecode, fn_id);
        const TaskEntry entry{ static_cast<uint32_t>(img->bytecode->size()),
                               iso->input.data(), iso->input.size(), world, iso.get() };
        Heap               heap(iso->actor ? ACTOR_INITIAL_SEMI : Heap::INITIAL_SEMI);
        StringInterner     interner;
        std::istringstream in;   // an isolate reads no stdin: it shares the process with the root
        auto res = execute(code, &heap, nullptr, &interner, TASK_STUB_FRAME,
                           img->const_pool, img->struct_types, img->string_literals,
                           img->atom_names, img->fn_table, &out,
                           img->trait_table, img->trait_table_width, img->trait_method_count,
                           img->line_table, img->function_names, img->column_table,
                           img->native_table, img->script_args, &in,
                           img->function_modules, img->const_arrays, &entry);
        if (!iso->actor) {
            iso->output = vcodec::encode(res.get_reg_base()[0]);
            iso->ok     = true;
        }
    } catch (const std::exception& e) {
        faulted = true;
        fault   = e.what();
    } catch (...) {
        faulted = true;
        fault   = "the isolate failed with an unknown exception";
    }
    if (!iso->actor) {
        if (faulted) iso->error = std::move(fault);
        iso->printed = task_out.str();
        return;
    }
    actor_buf.flush_partial();
    world->close_mailboxes_of(iso->id);   // the main inbox and every extra one (a slot is vacated)
    // The report names the ADDRESS, which for an actor in a slot outlives it -- so a supervisor's
    // comparison survives a restart. It goes to the starter's main inbox, found through its record:
    // its id is not its inbox's when the starter itself lives in a slot.
    if (faulted) {
        std::shared_ptr<Mailbox> box;
        if (iso->starter == 0) box = world->mailbox_of(0);
        else if (auto st = world->find(iso->starter)) box = st->mailbox;
        if (box) box->put(Mail{ MAIL_EXITED, {}, iso->mailbox_id, std::move(fault) });
    }
}

// The id inside a handle: a bare Int, or Skarn's one-field `Task[R]` / `Pid[M]` / `Inbox[M]`
// struct holding it (the checker hands the struct over so these natives can be typed by it).
static Value handle_id(Value v) {
    if (v.isPtr()) {
        const GcObject* o = GcObject::from_slots(v.asPtr());
        if (o->kind == GcObject::KIND_OBJECT && o->slot_count() >= 1) return o->slots()[0];
    }
    return v;
}

// Start an isolate: encode the argument HERE, in the starter, before the thread exists (so the
// starter may go on changing its own copy), start the thread, THEN register the record.
//
// Registering last is what makes the world's final join safe. The world ends only when the root
// finishes, and it joins every registered isolate -- including the one running this native, which
// therefore registers the new record before the world can be done with it; the next pass of the
// world's join loop finds it. Registering first would let that loop see a record whose `thread` is
// still being assigned. Nothing else needs the record before spawn returns its id.
//
// `into` is a SLOT the actor takes over instead of getting a mailbox of its own (rawSpawnInto): the
// slot keeps its id, so the new actor answers at the address its predecessor had.
static int64_t start_isolate(Value* args, uint8_t nargs, Context* ctx, bool actor, const char* who,
                             size_t capacity = 0, std::shared_ptr<Mailbox> into = nullptr,
                             int64_t into_id = 0) {
    VM* vm = ctx->vm;
    IsolateLocal* local = vm->isolate;
    if (!local || !local->world)
        raise_located(ctx, (std::string(who) + ": this execution cannot start isolates").c_str());
    if (nargs < 2 || !args[0].isFunc())
        raise_located(ctx, (std::string(who) + ": the function must be a named top-level function").c_str());
    const uint32_t fn_id = args[0].asFuncId();
    if (fn_id >= vm->fn_table_size || vm->fn_table[fn_id].arity != 1)
        raise_located(ctx, (std::string(who) + ": the function must take exactly one argument").c_str());
    World* world = local->world;
    auto iso = std::make_shared<Isolate>();
    iso->actor   = actor;
    iso->starter = local->id;
    try {
        iso->input = vcodec::encode(args[1]);
    } catch (const vcodec::ValueCodecError& e) {
        raise_located(ctx, (std::string(who) + ": the argument cannot be sent: " + e.what()).c_str());
    }
    if (actor) {
        iso->mailbox = into ? into : std::make_shared<Mailbox>();
        if (!into) iso->mailbox->capacity = capacity;
        world->actors_started.store(true, std::memory_order_release);
    }
    {
        // The main inbox is registered together with the id, before the thread exists: the actor may
        // receive at once, and a world ending from now on finds it (born while the world ends: Stop
        // at once, in add_mailbox_locked). A slot is registered already and keeps its id.
        std::lock_guard<std::mutex> lk(world->m);
        iso->id = world->next_id++;
        if (actor && into) {
            iso->mailbox_id = into_id;
            std::lock_guard<std::mutex> box_lk(into->m);
            into->owner = iso->id;
        } else if (actor) {
            iso->mailbox_id       = iso->id;
            iso->mailbox->owner   = iso->id;
            world->add_mailbox_locked(iso->id, iso->mailbox);
        }
    }
    try {
        iso->thread = std::thread(run_isolate, world, iso, static_cast<uint16_t>(fn_id));
    } catch (const std::system_error& e) {
        if (actor && into) into->vacate();
        else if (actor) world->close_mailbox(iso->id);
        raise_located(ctx, (std::string(who) + ": could not start a thread: " + e.what()).c_str());
    }
    {
        std::lock_guard<std::mutex> lk(world->m);
        world->isolates.emplace(iso->id, iso);
    }
    return iso->id;
}

// rawSpawn(fn, arg) -> Int, the task id.
static Value native_task_spawn(Value* args, uint8_t nargs, Context* ctx) {
    return Value::fromSigned48(start_isolate(args, nargs, ctx, /*actor=*/false, "spawn"));
}

// The task a handle names. Faults (located) on anything that is not a task THIS isolate started --
// a Task cannot be sent, so its starter is the only isolate that can hold it.
static std::shared_ptr<Isolate> own_task(Value h, Context* ctx) {
    const Value id = handle_id(h);
    IsolateLocal* local = ctx->vm->isolate;
    std::shared_ptr<Isolate> iso =
        (local && local->world && id.isInt()) ? local->world->find(id.asSigned48()) : nullptr;
    if (!iso || iso->actor || iso->starter != local->id)
        raise_located(ctx, "join: not a task of this program");
    return iso;
}

// rawJoin(id) -> Bool. Waits for the task; true iff it returned a value. What it printed is
// written to this isolate's output HERE, in join order -- so output does not depend on how the
// threads were scheduled.
static Value native_task_join(Value* args, uint8_t nargs, Context* ctx) {
    auto iso = own_task(nargs >= 1 ? args[0] : Value::fromNil(), ctx);
    if (iso->claimed)
        raise_located(ctx, "join: this task was already joined");
    iso->claimed = true;
    iso->join_thread();
    if (!iso->printed.empty()) {
        *ctx->vm->out << iso->printed;
        iso->printed.clear();
    }
    return Value::fromBool(iso->ok);
}

// rawTaskTake(id) -> the task's result, decoded into THIS heap, or its failure message. Once per
// task, after rawJoin; the buffers are released afterwards. The decoded value is held by the pool
// until the return, and by the register CALL_NATIVE writes it to after -- nothing allocates in
// between.
static Value native_task_take(Value* args, uint8_t nargs, Context* ctx) {
    auto iso = own_task(nargs >= 1 ? args[0] : Value::fromNil(), ctx);
    if (!iso->claimed || iso->taken)
        raise_located(ctx, "join: the task's result is not available");
    iso->taken = true;
    if (!iso->ok) {
        const std::string msg = std::move(iso->error);
        return native_make_error(ctx, msg);
    }
    const std::vector<uint8_t> buf = std::move(iso->output);
    try {
        RootedValuePool pool;
        return vcodec::decode(buf.data(), buf.size(), *ctx->vm->heap, ctx, pool,
                              ctx->vm->struct_types, ctx->vm->struct_type_count);
    } catch (const vcodec::ValueCodecError& e) {
        raise_located(ctx, (std::string("join: the result cannot be received: ") + e.what()).c_str());
    }
}

// rawTaskInput() -> the isolate's argument, decoded into its own heap. Called once, by the entry
// stub; anywhere else it faults.
static Value native_task_input(Value*, uint8_t, Context* ctx) {
    VM* vm = ctx->vm;
    if (!vm->task_input)
        raise_located(ctx, "rawTaskInput: this execution is not a task or an actor");
    try {
        RootedValuePool pool;
        return vcodec::decode(vm->task_input, vm->task_input_size, *vm->heap, ctx, pool,
                              vm->struct_types, vm->struct_type_count);
    } catch (const vcodec::ValueCodecError& e) {
        raise_located(ctx, (std::string("task: the argument cannot be received: ") + e.what()).c_str());
    }
}

// rawSelfId() -> Int: this isolate's id (0 for the root). An actor's trampoline builds its
// Inbox from it.
static Value native_self_id(Value*, uint8_t, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    return Value::fromSigned48(local ? local->id : 0);
}

// rawSpawnActor(fn, arg) -> Int, the actor id.
static Value native_actor_spawn(Value* args, uint8_t nargs, Context* ctx) {
    return Value::fromSigned48(start_isolate(args, nargs, ctx, /*actor=*/true, "spawnActor"));
}

// rawSpawnActorBounded(fn, arg, capacity) -> Int: an actor whose main inbox holds at most
// `capacity` messages; a send to it waits while it is full (back-pressure).
static Value native_actor_spawn_bounded(Value* args, uint8_t nargs, Context* ctx) {
    const Value cap = nargs >= 3 ? args[2] : Value::fromNil();
    if (!cap.isInt() || cap.asSigned48() < 1)
        raise_located(ctx, "spawnActorBounded: the capacity must be at least 1");
    return Value::fromSigned48(start_isolate(args, 2, ctx, /*actor=*/true, "spawnActorBounded",
                                             static_cast<size_t>(cap.asSigned48())));
}

// rawNewSlot(capacity) -> Int: an ADDRESS with no actor yet. Messages sent to it wait until an actor
// is started into it (rawSpawnInto), and it survives that actor -- which is how a restarted actor
// keeps the address its predecessor had. Anyone may make one; only holding its id means anything.
static Value native_new_slot(Value* args, uint8_t nargs, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    if (!local || !local->world)
        raise_located(ctx, "newSlot: this execution has no world");
    const Value cap = nargs >= 1 ? args[0] : Value::fromNil();
    if (!cap.isInt() || cap.asSigned48() < 0)
        raise_located(ctx, "newSlot: the capacity must be 0 (unbounded) or more");
    auto box = std::make_shared<Mailbox>();
    box->slot     = true;
    box->owner    = Mailbox::NO_OWNER;
    box->capacity = static_cast<size_t>(cap.asSigned48());
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(local->world->m);
        id = local->world->next_id++;
        local->world->add_mailbox_locked(id, box);
    }
    return Value::fromSigned48(id);
}

// rawSpawnInto(slot, fn, arg) -> Int, the actor id. The actor receives on the slot instead of on a
// mailbox of its own, so it answers at the slot's address. One actor at a time.
static Value native_spawn_into(Value* args, uint8_t nargs, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    if (!local || !local->world)
        raise_located(ctx, "spawnInto: this execution cannot start isolates");
    if (nargs < 3)
        raise_located(ctx, "spawnInto: needs a slot, a function and a start value");
    const Value id = handle_id(args[0]);
    if (!id.isInt()) raise_located(ctx, "spawnInto: not an address");
    auto box = local->world->mailbox_of(id.asSigned48());
    if (!box || !box->slot) raise_located(ctx, "spawnInto: this address is not a slot, or it was released");
    {
        std::lock_guard<std::mutex> lk(box->m);
        if (box->owner != Mailbox::NO_OWNER)
            raise_located(ctx, "spawnInto: this address already has an actor");
    }
    return Value::fromSigned48(start_isolate(args + 1, static_cast<uint8_t>(nargs - 1), ctx,
                                             /*actor=*/true, "spawnInto", 0, box, id.asSigned48()));
}

// rawReleaseSlot(slot) -> (). Ends an address: later sends answer false, and an actor still running
// in it is told to stop (it ends when it reads its mail, as at the end of the program).
static Value native_release_slot(Value* args, uint8_t nargs, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    if (!local || !local->world) raise_located(ctx, "release: this execution has no world");
    const Value id = handle_id(nargs >= 1 ? args[0] : Value::fromNil());
    if (!id.isInt()) raise_located(ctx, "release: not an address");
    if (auto box = local->world->mailbox_of(id.asSigned48()); box && box->slot) {
        local->world->close_mailbox(id.asSigned48());   // out of the registry first: no later send gets in
        box->retire();
    }
    return Value::fromNil();
}

// rawStopActor(pid) -> Bool. Tells an actor to end: Stop into EVERY inbox it owns, because it may be
// waiting on any of them. False if it no longer runs. Its own return is what actually ends it -- an
// actor that never receives cannot be stopped from outside.
static Value native_stop_actor(Value* args, uint8_t nargs, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    if (!local || !local->world) raise_located(ctx, "stopActor: this execution has no world");
    const Value id = handle_id(nargs >= 1 ? args[0] : Value::fromNil());
    if (!id.isInt()) raise_located(ctx, "stopActor: not an actor address");
    auto box = local->world->mailbox_of(id.asSigned48());
    if (!box) return Value::fromBool(false);
    int64_t owner;
    { std::lock_guard<std::mutex> lk(box->m); owner = box->owner; }
    if (owner == Mailbox::NO_OWNER || owner == 0) return Value::fromBool(false);
    for (auto& b : local->world->mailboxes_of(owner)) b->put(Mail{ MAIL_STOP, {}, 0, {} });
    return Value::fromBool(true);
}

// rawMainInbox() -> Int. Gives the ROOT a mailbox, so it can receive replies and crash reports.
// ONCE: the Inbox it becomes fixes the message type, and a second one of another type could read
// the same queue as something else.
static Value native_main_inbox(Value*, uint8_t, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    if (!local || !local->world || local->id != 0)
        raise_located(ctx, "mainInbox: only the main program has a main inbox");
    {
        std::lock_guard<std::mutex> lk(local->world->m);
        if (local->world->main_inbox_taken)
            raise_located(ctx, "mainInbox: the main inbox was already taken");
        local->world->main_inbox_taken = true;
    }
    local->inboxes.emplace_back(0, local->world->main_mailbox);
    return Value::fromSigned48(0);
}

// rawNewInbox(capacity) -> Int, the id of a new inbox THIS isolate owns: a second address, typically
// for replies, so that they neither mix with nor wait behind the main inbox's mail. Capacity 0 =
// unbounded. For actors and the root; a task has no mail at all. The inbox lives until rawCloseInbox
// or the end of its owner.
static Value native_new_inbox(Value* args, uint8_t nargs, Context* ctx) {
    IsolateLocal* local = ctx->vm->isolate;
    if (!local || !local->world || (local->id != 0 && !local->actor))
        raise_located(ctx, "newInbox: only an actor or the main program can have an inbox");
    const Value cap = nargs >= 1 ? args[0] : Value::fromNil();
    if (!cap.isInt() || cap.asSigned48() < 0)
        raise_located(ctx, "newInbox: the capacity must be 0 (unbounded) or more");
    auto box = std::make_shared<Mailbox>();
    box->owner    = local->id;
    box->capacity = static_cast<size_t>(cap.asSigned48());
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(local->world->m);
        id = local->world->next_id++;
        local->world->add_mailbox_locked(id, box);
    }
    local->inboxes.emplace_back(id, std::move(box));
    return Value::fromSigned48(id);
}

// This isolate's inbox that `inbox` names. An Inbox cannot be sent, so an inbox of another isolate
// means a forged one; one that is not open any more was closed.
static Mailbox& own_mailbox(Value inbox, Context* ctx, const char* who) {
    IsolateLocal* local = ctx->vm->isolate;
    const Value   id    = handle_id(inbox);
    // Not an open inbox of ours. A task has none at all, and the root none at 0 before mainInbox;
    // an open one of someone else is a forged Inbox; anything else was closed (the closing isolate
    // keeps no record of it -- an actor making an inbox per request would grow it without end).
    const bool has_mail = local && local->world && (local->actor || local->id == 0);
    if (has_mail && id.isInt()) {
        const int64_t i = id.asSigned48();
        if (Mailbox* box = local->inbox(i)) return *box;
        if (auto box = local->world->mailbox_of(i); box && box->owner != local->id)
            raise_located(ctx, (std::string(who) + ": this inbox belongs to another actor").c_str());
        if (!(i == 0 && local->id == 0))
            raise_located(ctx, (std::string(who) + ": this inbox is closed").c_str());
    }
    raise_located(ctx, (std::string(who) + ": this execution has no inbox (mainInbox first?)").c_str());
}

// rawCloseInbox(inbox) -> (). Closes an extra inbox of this isolate: later sends to it answer false
// and whatever is queued is dropped -- a late reply nobody waits for any more cannot pile up. The
// MAIN inbox cannot be closed: exit reports and Stop arrive there, and it ends with its owner.
static Value native_close_inbox(Value* args, uint8_t nargs, Context* ctx) {
    (void)own_mailbox(nargs >= 1 ? args[0] : Value::fromNil(), ctx, "close");
    IsolateLocal* local = ctx->vm->isolate;
    const int64_t id    = handle_id(args[0]).asSigned48();
    if (id == local->main_box)
        raise_located(ctx, "close: the main inbox cannot be closed (it ends with its owner)");
    local->world->close_mailbox(id);
    std::erase_if(local->inboxes, [id](const auto& e) { return e.first == id; });
    return Value::fromNil();
}

// The message of a send, encoded HERE, in the sender's heap -- before any waiting, so a sender
// blocked on a full inbox holds no heap object.
static Mail encode_message(Value* args, uint8_t nargs, Context* ctx, const char* who) {
    Mail mail;
    mail.kind = MAIL_MSG;
    try {
        mail.buf = vcodec::encode(nargs >= 2 ? args[1] : Value::fromNil());
    } catch (const vcodec::ValueCodecError& e) {
        raise_located(ctx, (std::string(who) + ": the message cannot be sent: " + e.what()).c_str());
    }
    return mail;
}
static std::shared_ptr<Mailbox> addressee(Value* args, uint8_t nargs, Context* ctx, const char* who) {
    IsolateLocal* local = ctx->vm->isolate;
    const Value   id    = handle_id(nargs >= 1 ? args[0] : Value::fromNil());
    if (!local || !local->world || !id.isInt() || nargs < 2)
        raise_located(ctx, (std::string(who) + ": not an actor address").c_str());
    return local->world->mailbox_of(id.asSigned48());
}

// rawSend(pid, msg) -> Bool. False when the addressee no longer runs, its inbox was closed, or the
// program is ending while the inbox is full; the message is then dropped, as in Erlang. To a FULL
// bounded inbox it waits until there is room (back-pressure). Waiting on one's own full inbox could
// never end -- only this isolate empties it -- so that is a located fault instead.
static Value native_send(Value* args, uint8_t nargs, Context* ctx) {
    auto box  = addressee(args, nargs, ctx, "send");
    Mail mail = encode_message(args, nargs, ctx, "send");
    if (!box) return Value::fromBool(false);
    if (box->capacity != 0 && box->owner == ctx->vm->isolate->id) {
        if (box->offer(std::move(mail), /*wait=*/false) == Mailbox::FULL)
            raise_located(ctx, "send: this actor's own inbox is full, and waiting for room would never end");
        return Value::fromBool(true);   // SENT (GONE is impossible: the owner is running)
    }
    return Value::fromBool(box->offer(std::move(mail), /*wait=*/true) == Mailbox::SENT);
}

// rawTrySend(pid, msg) -> Int: 0 sent, 1 the inbox is full (nothing was queued), 2 the addressee is
// gone. Never waits.
static Value native_try_send(Value* args, uint8_t nargs, Context* ctx) {
    auto box  = addressee(args, nargs, ctx, "trySend");
    Mail mail = encode_message(args, nargs, ctx, "trySend");
    if (!box) return Value::fromSigned48(2);
    switch (box->offer(std::move(mail), /*wait=*/false)) {
        case Mailbox::SENT: return Value::fromSigned48(0);
        case Mailbox::FULL: return Value::fromSigned48(1);
        case Mailbox::GONE: return Value::fromSigned48(2);
    }
    return Value::fromSigned48(2);
}

// rawReceive(inbox, timeoutMs) -> Int: MAIL_NONE (the timeout passed), MAIL_MSG, MAIL_EXITED or
// MAIL_STOP. A negative timeout waits indefinitely. The mail itself is kept as the isolate's
// `current`, for rawMailMsg / rawMailFrom / rawMailReason. Once Stop has been received, every
// later receive answers Stop at once. Blocking here holds no VM lock and touches no heap.
static Value native_receive(Value* args, uint8_t nargs, Context* ctx) {
    Mailbox& box = own_mailbox(nargs >= 1 ? args[0] : Value::fromNil(), ctx, "receive");
    const int64_t ms = (nargs >= 2 && args[1].isInt()) ? args[1].asSigned48() : -1;
    IsolateLocal* local = ctx->vm->isolate;
    std::unique_lock<std::mutex> lk(box.m);
    auto ready = [&] { return !box.q.empty() || box.stop_seen; };
    if (ms < 0) box.cv.wait(lk, ready);
    else        box.cv.wait_for(lk, std::chrono::milliseconds(ms), ready);
    if (box.q.empty()) {
        local->current = Mail{ box.stop_seen ? MAIL_STOP : MAIL_NONE, {}, 0, {} };
        return Value::fromSigned48(local->current.kind);
    }
    local->current = std::move(box.q.front());
    box.q.pop_front();
    if (local->current.kind == MAIL_STOP) box.stop_seen = true;
    if (local->current.kind == MAIL_MSG) {
        --box.messages;
        if (box.capacity != 0) { lk.unlock(); box.space.notify_one(); }   // room for one sender
    }
    return Value::fromSigned48(local->current.kind);
}

// rawMailMsg(inbox) -> the message rawReceive took, decoded into THIS heap. Once per message.
static Value native_mail_msg(Value* args, uint8_t nargs, Context* ctx) {
    (void)own_mailbox(nargs >= 1 ? args[0] : Value::fromNil(), ctx, "receive");
    Mail& cur = ctx->vm->isolate->current;
    if (cur.kind != MAIL_MSG)
        raise_located(ctx, "receive: there is no message to read");
    const std::vector<uint8_t> buf = std::move(cur.buf);
    cur.kind = MAIL_NONE;
    try {
        RootedValuePool pool;
        return vcodec::decode(buf.data(), buf.size(), *ctx->vm->heap, ctx, pool,
                              ctx->vm->struct_types, ctx->vm->struct_type_count);
    } catch (const vcodec::ValueCodecError& e) {
        raise_located(ctx, (std::string("receive: the message cannot be received: ") + e.what()).c_str());
    }
}

// rawMailFrom(inbox) -> Int, and rawMailReason(inbox) -> String: the actor a MAIL_EXITED reports,
// and its fault message.
static Value native_mail_from(Value* args, uint8_t nargs, Context* ctx) {
    (void)own_mailbox(nargs >= 1 ? args[0] : Value::fromNil(), ctx, "receive");
    const Mail& cur = ctx->vm->isolate->current;
    if (cur.kind != MAIL_EXITED)
        raise_located(ctx, "receive: there is no exit report to read");
    return Value::fromSigned48(cur.from);
}
static Value native_mail_reason(Value* args, uint8_t nargs, Context* ctx) {
    (void)own_mailbox(nargs >= 1 ? args[0] : Value::fromNil(), ctx, "receive");
    const Mail& cur = ctx->vm->isolate->current;
    if (cur.kind != MAIL_EXITED)
        raise_located(ctx, "receive: there is no exit report to read");
    const std::string reason = cur.reason;
    return native_make_error(ctx, reason);
}

// rawHandOff(fd) -> Int ticket | String. Moves a connection OUT of this isolate: the socket leaves
// this NetRegistry (it is not closed) and waits in the world under a fresh ticket until an isolate
// takes it. The descriptor goes stale at once, so a later use of it is refused with "socket was
// handed to another actor" (see NetRegistry). The ticket is plain data and travels in a message.
static Value native_hand_off(Value* args, uint8_t nargs, Context* ctx) {
    VM* vm = ctx->vm;
    if (!vm->net) return native_make_error(ctx, "handOff: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "handOff: expected (sock: Int)");
    IsolateLocal* local = vm->isolate;
    if (!local || !local->world) return native_make_error(ctx, "handOff: this execution has no world");
    const int64_t fd = args[0].asSigned48();
    const socket_t s = vm->net->hand_off(fd);
    if (s == INVALID_SOCK)
        return native_make_error(ctx, std::string("handOff: ") + vm->net->invalid_reason(fd));
    std::lock_guard<std::mutex> lk(local->world->m);
    const int64_t ticket = local->world->next_ticket++;
    local->world->handoffs.emplace(ticket, s);
    return Value::fromSigned48(ticket);
}

// rawTake(ticket) -> Int fd | String. Moves a handed-off connection INTO this isolate's NetRegistry.
// Once per ticket: a second take, or a ticket never issued, is an error.
static Value native_take(Value* args, uint8_t nargs, Context* ctx) {
    VM* vm = ctx->vm;
    if (!vm->net) return native_make_error(ctx, "take: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "take: expected (ticket: Int)");
    IsolateLocal* local = vm->isolate;
    if (!local || !local->world) return native_make_error(ctx, "take: this execution has no world");
    socket_t s = INVALID_SOCK;
    {
        std::lock_guard<std::mutex> lk(local->world->m);
        auto it = local->world->handoffs.find(args[0].asSigned48());
        if (it != local->world->handoffs.end()) {
            s = it->second;
            local->world->handoffs.erase(it);
        }
    }
    if (s == INVALID_SOCK) return native_make_error(ctx, "take: this connection was already taken");
    return Value::fromSigned48(vm->net->add(s));
}

std::vector<NativeFunc> build_native_table() {
    std::vector<NativeFunc> t(NATIVE_COUNT, nullptr);
    t[NATIVE_READ_FILE]   = native_read_file;
    t[NATIVE_WRITE_FILE]  = native_write_file;
    t[NATIVE_NANO_TIME]   = native_nano_time;
    t[NATIVE_MILLIS_TIME] = native_millis_time;
    t[NATIVE_ARGS]        = native_args;
    t[NATIVE_GET_ENV]     = native_get_env;
    t[NATIVE_FILE_EXISTS] = native_file_exists;
    t[NATIVE_DELETE_FILE] = native_delete_file;
    t[NATIVE_LIST_DIR]    = native_list_dir;
    t[NATIVE_MAKE_DIR]    = native_make_dir;
    t[NATIVE_READ_LINE]   = native_read_line;
    t[NATIVE_READ_ALL_STDIN] = native_read_all_stdin;
    t[NATIVE_RUN_PROCESS] = native_run_process;
    t[NATIVE_PARSE_INT]   = native_parse_int;
    t[NATIVE_PARSE_DOUBLE] = native_parse_double;
    t[NATIVE_RAW_GC_STATS] = native_raw_gc_stats;
    t[NATIVE_GC_RESET_STATS] = native_gc_reset_stats;
    t[NATIVE_APPEND_FILE] = native_append_file;
    t[NATIVE_IS_FILE]     = native_is_file;
    t[NATIVE_IS_DIR]      = native_is_dir;
    t[NATIVE_FILE_SIZE]   = native_file_size;
    t[NATIVE_RENAME]      = native_rename;
    t[NATIVE_COPY_FILE]   = native_copy_file;
    t[NATIVE_SQRT]  = native_sqrt;   t[NATIVE_CBRT]  = native_cbrt;  t[NATIVE_POW]   = native_pow;
    t[NATIVE_HYPOT] = native_hypot;  t[NATIVE_EXP]   = native_exp;   t[NATIVE_LN]    = native_ln;
    t[NATIVE_LOG2]  = native_log2;   t[NATIVE_LOG10] = native_log10; t[NATIVE_SIN]   = native_sin;
    t[NATIVE_COS]   = native_cos;    t[NATIVE_TAN]   = native_tan;   t[NATIVE_ASIN]  = native_asin;
    t[NATIVE_ACOS]  = native_acos;   t[NATIVE_ATAN]  = native_atan;  t[NATIVE_ATAN2] = native_atan2;
    t[NATIVE_FABS]  = native_fabs;   t[NATIVE_ISNAN] = native_isnan; t[NATIVE_ISINF] = native_isinf;
    t[NATIVE_F64_TO_BYTES] = native_f64_to_bytes;
    t[NATIVE_TCP_CONNECT]  = native_tcp_connect;
    t[NATIVE_TCP_SEND]     = native_tcp_send;
    t[NATIVE_TCP_RECV]     = native_tcp_recv;
    t[NATIVE_TCP_CLOSE]    = native_tcp_close;
    t[NATIVE_TCP_LISTEN]   = native_tcp_listen;
    t[NATIVE_TCP_ACCEPT]   = native_tcp_accept;
    t[NATIVE_TCP_SET_TIMEOUT] = native_tcp_set_timeout;
    t[NATIVE_TCP_LOCAL_PORT] = native_tcp_local_port;
    t[NATIVE_SHA256]       = native_sha256;
    t[NATIVE_OS_ID]        = native_os_id;
    t[NATIVE_SET_NON_BLOCKING] = native_raw_set_non_blocking;
    t[NATIVE_POLL]         = native_raw_poll;
    t[NATIVE_ACCEPT_NB]    = native_raw_accept_nb;
    t[NATIVE_RECV_NB]      = native_raw_recv_nb;
    t[NATIVE_SEND_NB]      = native_raw_send_nb;
    t[NATIVE_TASK_SPAWN]   = native_task_spawn;
    t[NATIVE_TASK_JOIN]    = native_task_join;
    t[NATIVE_TASK_TAKE]    = native_task_take;
    t[NATIVE_TASK_INPUT]   = native_task_input;
    t[NATIVE_ACTOR_SPAWN]  = native_actor_spawn;
    t[NATIVE_SEND]         = native_send;
    t[NATIVE_RECEIVE]      = native_receive;
    t[NATIVE_MAIL_MSG]     = native_mail_msg;
    t[NATIVE_MAIL_FROM]    = native_mail_from;
    t[NATIVE_MAIL_REASON]  = native_mail_reason;
    t[NATIVE_MAIN_INBOX]   = native_main_inbox;
    t[NATIVE_SELF_ID]      = native_self_id;
    t[NATIVE_HAND_OFF]     = native_hand_off;
    t[NATIVE_TAKE]         = native_take;
    t[NATIVE_NEW_INBOX]    = native_new_inbox;
    t[NATIVE_CLOSE_INBOX]  = native_close_inbox;
    t[NATIVE_TRY_SEND]     = native_try_send;
    t[NATIVE_ACTOR_SPAWN_BOUNDED] = native_actor_spawn_bounded;
    t[NATIVE_NEW_SLOT]     = native_new_slot;
    t[NATIVE_SPAWN_INTO]   = native_spawn_into;
    t[NATIVE_RELEASE_SLOT] = native_release_slot;
    t[NATIVE_STOP_ACTOR]   = native_stop_actor;
    return t;
}
