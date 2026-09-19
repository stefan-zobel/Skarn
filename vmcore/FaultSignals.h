#pragma once
#ifndef _WIN32
// =============================================================================
// FaultSignals.h -- the POSIX half of run_switch's fault frame.
//
// WHAT THIS REPLACES. On Windows, run_switch wraps the dispatch loop in one SEH
// __try/__except. That single construct does three things at once: it decides whether a
// fault belongs to the VM (the dynamic extent of the __try), it hands the handler the
// fault address (GetExceptionInformation), and it runs the recovery code in NORMAL
// context, where std::format and `throw` are legal.
//
// POSIX gives none of the three for free. A signal handler is a process-global entry
// point chosen by the kernel: it takes no argument, it can run on any thread, and almost
// nothing in this codebase may be called from it. So the one SEH construct splits into
// three pieces that have to be assembled by hand:
//
//   SEH piece                        POSIX equivalent              lives in
//   -------------------------------  ----------------------------  -----------------
//   __try scope = "is this ours?"    an ARMED WINDOW published      run_switch (RAII)
//                                    around run_switch_loop
//   GetExceptionInformation()        siginfo_t::si_addr, copied     the handler
//                                    into the armed window
//   __except body (may throw)        the sigsetjmp(...) != 0 arm    run_switch
//   unwind to the __try frame        siglongjmp                     the handler
//
// WHY THE RENDEZVOUS IS A GLOBAL ATOMIC AND NOT thread_local. TLS is the reflex answer
// and it is the wrong one on the target platform. Darwin has no local-exec TLS model:
// every thread_local access goes through a TLV descriptor, and a thread's FIRST access
// calls malloc to allocate its TLV block. A faulting pipe-drainer thread (rawRun spawns
// two) would therefore call malloc *inside a SIGSEGV handler* -- against, quite possibly,
// the very malloc lock the faulting thread is holding. A std::atomic<Landing*> load is a
// single instruction with no descriptor and no allocation.
//
// This is also a far smaller re-introduction of global state than it looks. vmcore.cpp's
// comment above `VM vm;` records that per-execution state was deliberately moved OUT of
// TLS hooks and into Context. That decision stands untouched: VM, VM_Resources and
// Context remain stack locals in execute(). What is global here is one POINTER to a frame
// that provably lives exactly as long as the armed window -- and a handler has no
// argument channel, so a rendezvous slot is not a design preference, it is the minimum
// the mechanism requires.
// =============================================================================

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>      // getenv
#include <pthread.h>
#include <sys/mman.h>

#if defined(__APPLE__) && defined(__aarch64__)
#  include <sys/ucontext.h>
#endif

namespace vm_signals {

// -----------------------------------------------------------------------------
// The rendezvous between run_switch and the fault handler. ONE of these lives in each
// run_switch frame; only the pointer to the innermost is global.
// -----------------------------------------------------------------------------
struct Landing {
    sigjmp_buf jb;                        // sigsetjmp(jb, /*savemask=*/1)
    pthread_t  owner{};                   // the thread that armed it
    Landing*   prev = nullptr;            // enclosing window, for nested execute()
    void* volatile        addr     = nullptr;   // si_addr, written by the handler
    volatile sig_atomic_t signo    = 0;         // SIGSEGV or SIGBUS
    volatile sig_atomic_t is_write = 0;         // 1 = store, 0 = load or unknown
    bool                  armed    = false;     // normal-context bookkeeping only
};

inline std::atomic<Landing*> g_landing{nullptr};
static_assert(std::atomic<Landing*>::is_always_lock_free,
              "the rendezvous load must be lock-free: it runs inside a signal handler");

// The dispositions we displaced, so an unrelated fault can be handed back to whatever
// was there before (usually SIG_DFL, but possibly a debugger or an embedder's handler).
inline struct sigaction g_old_segv {};
inline struct sigaction g_old_bus  {};

// -----------------------------------------------------------------------------
// Arming / retracting. Save-on-entry, restore-on-exit -- the same shape the removed TLS
// hooks needed, except the saved value lives in the caller's frame rather than in a
// second global.
// -----------------------------------------------------------------------------
inline void retract(Landing& L) noexcept {
    if (L.armed) {
        L.armed = false;
        g_landing.store(L.prev, std::memory_order_release);
    }
}

struct Arm {
    Landing& L;
    explicit Arm(Landing& l) noexcept : L(l) {
        L.owner = pthread_self();
        L.prev  = g_landing.load(std::memory_order_relaxed);
        // Two threads inside execute() at once is not a supported configuration. If it
        // ever happens, refuse to arm rather than risk a cross-thread longjmp: the run
        // then simply gets no fault classification, exactly like before this header.
        if (L.prev && (void*)L.prev->owner != (void*)L.owner) {
            assert(false && "concurrent execute() on two threads is not supported");
            L.armed = false;
            return;
        }
        L.armed = true;
        g_landing.store(&L, std::memory_order_release);
    }
    ~Arm() { retract(L); }
    Arm(const Arm&)            = delete;
    Arm& operator=(const Arm&) = delete;
};

// -----------------------------------------------------------------------------
// The handler.
//
// ASYNC-SIGNAL-SAFETY -- the complete inventory of what runs below, because the next
// person to touch this will be tempted to "improve" it:
//
//   std::atomic<Landing*>::load   not a call; one lock-free load (static_assert above)
//   pthread_self()                on the POSIX async-signal-safe list
//   (void*) compare of pthread_t  not a call. Deliberately NOT pthread_equal, which is
//                                 NOT on the list.
//   reads of si_addr / __esr      plain loads from kernel-provided structs
//   stores into *L                plain aligned stores to a frame this thread owns; the
//                                 handler runs to completion before the jump target
//                                 resumes, so there is no race. `volatile` is for the
//                                 compiler, not the CPU.
//   sigaction()                   on the list (chain path only)
//   siglongjmp()                  on the list
//
// NOT called, and must never be: malloc/free, std::format, std::string, iostreams,
// throw, backtrace, printf, dladdr, ANY thread_local access, and -- though it would be
// safe in itself -- VM_Resources::address_in_stacks. Classification happens after the
// jump; pulling VM_Resources in here only creates the temptation to format a message.
//
// THE HONEST CAVEAT. POSIX lists siglongjmp as async-signal-safe, but also says
// behaviour is undefined if the signal interrupted a non-async-signal-safe function and
// the program then calls one. We do exactly that: the fault can land inside memcpy or
// inside malloc, and after the jump we call std::format. Two things make this the
// accepted technique rather than a gamble. First, SIGSEGV/SIGBUS from a bad access is
// SYNCHRONOUS and thread-directed -- it is the faulting instruction itself, not an
// arbitrary interruption. Second, the one real hazard is a lock held at the fault PC
// (realistically the malloc lock), which would deadlock the subsequent format. That is
// unavoidable with this technique -- and it is precisely the risk the Windows __except
// path already runs, reachable only from already-unrecoverable heap corruption.
// -----------------------------------------------------------------------------
inline void chain_to_previous(int signo) noexcept {
    const struct sigaction& old = (signo == SIGBUS) ? g_old_bus : g_old_segv;
    // Reinstate the previous disposition and RETURN. The faulting instruction re-executes
    // and re-faults under that disposition, which keeps the original PC and register
    // state -- so the crash report still points at the real bug rather than at us.
    sigaction(signo, &old, nullptr);
}

extern "C" inline void fault_handler(int signo, siginfo_t* info, void* uctx) {
    Landing* L = g_landing.load(std::memory_order_acquire);

    // Not our thread, or the VM is not dispatching. Hand it back. This check is
    // load-bearing, not hygiene: rawRun's two drainer threads are alive precisely while
    // the VM thread sits in run_switch with the window armed, and a longjmp from a
    // drainer would set that thread's SP into ANOTHER thread's stack.
    if (!L || (void*)L->owner != (void*)pthread_self()) {
        chain_to_previous(signo);
        return;
    }

    L->addr  = info ? info->si_addr : nullptr;
    L->signo = signo;

#if defined(__APPLE__) && defined(__aarch64__)
    // ESR_EL1 bit 6 (WnR) says store-vs-load, but only for a Data Abort: exception class
    // 0b100100 (lower EL) or 0b100101 (same EL). Anything else leaves is_write at 0, and
    // the message then says "read" -- the one cosmetic difference from the Windows path,
    // which gets the bit directly from ExceptionInformation[0].
    if (uctx) {
        const ucontext_t* uc  = static_cast<const ucontext_t*>(uctx);
        const uint64_t    esr = uc->uc_mcontext->__es.__esr;
        const uint64_t    ec  = esr >> 26;
        L->is_write = ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6))) ? 1 : 0;
    }
#else
    (void)uctx;   // no portable way to recover store-vs-load; reported as a read
#endif

    siglongjmp(L->jb, 1);   // does not return
}

// -----------------------------------------------------------------------------
// The alternate signal stack. Per thread, and installed from NORMAL context only -- which
// is why a thread_local flag is fine here and nowhere else in this header.
//
// Strictly this VM does not need an altstack: SA_ONSTACK earns its keep when the fault IS
// a native C-stack overflow, so the kernel has nowhere to push the signal frame, and this
// VM's faults are on mmap'd regions with a healthy C stack (native recursion is bounded
// everywhere -- value_deep_eq uses an explicit worklist, the dump helpers cap their
// depth). It is installed as defence in depth, and a failure to install is not fatal.
// -----------------------------------------------------------------------------
inline stack_t& altstack_slot() noexcept {
    static thread_local stack_t ss {};
    return ss;
}

inline void ensure_altstack() noexcept {
    stack_t& ss = altstack_slot();
    if (ss.ss_sp) return;                       // already installed on this thread
    size_t sz = 64 * 1024;
    if (sz < static_cast<size_t>(MINSIGSTKSZ)) sz = static_cast<size_t>(MINSIGSTKSZ);
    // mmap rather than malloc: never moved, never touched by the allocator, and
    // deliberately leaked for the process lifetime (a thread-exit cleanup would buy
    // nothing and add a destruction-order hazard).
    void* mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return;              // degrade: handler runs on the normal stack
    ss.ss_sp    = mem;
    ss.ss_size  = sz;
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
}

// Re-issue the alternate stack after a recovered fault.
//
// Darwin sets a STICKY "we are on the alternate stack" bit when it delivers the signal
// and clears it in sigreturn -- which we never reach, because we siglongjmp out. Without
// this, every fault after the first is delivered on the NORMAL stack. (Linux computes
// on-altstack from SP and self-heals, so this is a no-op there.) If the re-arm fails
// because the kernel still believes we are on the stack, that is survivable for the
// reason given above: this VM does not depend on the altstack.
inline void rearm_altstack() noexcept {
    stack_t& ss = altstack_slot();
    if (!ss.ss_sp) return;
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
}

// -----------------------------------------------------------------------------
// Installation. Process-once and never uninstalled: the ARMED WINDOW is what gates
// behaviour, not the disposition, and uninstalling at execute() exit would race any
// other thread for no gain.
// -----------------------------------------------------------------------------
inline bool install_once() noexcept {
    // Escape hatch for an embedder that manages its own signal handling. vmcore is a
    // static library, and installing a SIGSEGV handler changes behaviour for the whole
    // host process -- so the opt-out matters. (Installation is lazy, from run_switch, so
    // a host that never calls execute() is never affected either way.)
    if (std::getenv("SKARN_NO_FAULT_HANDLER")) return false;

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    // Never fight the sanitizer's own handler: installing over it destroys its reporting,
    // which is strictly more valuable than ours.
    return false;
#  endif
#endif

    struct sigaction sa {};
    sa.sa_sigaction = &fault_handler;
    sigemptyset(&sa.sa_mask);
    // SA_NODEFER is deliberately NOT set. The delivered signal stays blocked inside the
    // handler, so a fault WITHIN the handler (a corrupt g_landing, say) is force-delivered
    // with the default disposition and kills the process immediately instead of recursing.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

    // BOTH signals, and SIGBUS is not optional on Darwin: KERN_INVALID_ADDRESS maps to
    // SIGSEGV but KERN_PROTECTION_FAILURE maps to SIGBUS -- and VM_Resources reserves with
    // mmap(PROT_NONE), so a guard-page hit (the case this whole mechanism exists for)
    // arrives as SIGBUS there and as SIGSEGV on Linux.
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS,  &sa, &g_old_bus);
    return true;
}

inline bool ensure_installed() noexcept {
    static const bool ok = install_once();   // magic static: exactly once per process
    if (ok) ensure_altstack();               // per thread
    return ok;
}

} // namespace vm_signals

#endif // !_WIN32
