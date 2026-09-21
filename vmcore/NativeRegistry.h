#pragma once

// =============================================================================
// NativeRegistry.h -- the shared name<->id registry for the built-in native
// functions, the single source of truth both the compiler and the VM read
// (like Opcodes.h / TypeUniverse.h). The compiler lowers a native builtin
// (readFile / writeFile) to `LOAD_CONST id_reg, id` + CALL_NATIVE; the VM's
// CALL_NATIVE indexes VM::native_table[id] to recover the function pointer.
//
// Why an id (and a VM-side table) rather than baking a runtime address into the
// bytecode: an address is an ASLR-randomized RUNTIME fact -- baked bytecode would be
// valid only in the assembling process and could not be serialized, and the compiler
// cannot produce an address at compile time at all. A small stable id is both. (The
// legacy baked-pointer path -- Assembler::load_native_ptr / call_native -- was retired,
// so CALL_NATIVE is single-mode: every caller passes a native_table.)
// See "Native functions" in docs/VirtualMachine.md.
//
// Kept deliberately light (no Value / Context / heap types) so the COMPILER can
// include it for native_id_of() without pulling in any VM-runtime header.
// =============================================================================

#include <cstdint>
#include <string>

// Dense, stable ids for the built-in natives. The order defines the layout of
// the native_table the driver builds (build_native_table, Natives.h) and passes
// to execute(). APPEND ONLY -- never renumber (an id is baked into bytecode).
enum NativeId : uint16_t {
    NATIVE_READ_FILE   = 0,   // readFile(path)         -> Bytes  | String (error message)
    NATIVE_WRITE_FILE  = 1,   // writeFile(path, bytes) -> nil    | String (error message)
    NATIVE_NANO_TIME   = 2,   // nanoTime()             -> Int (monotonic, offset; Plain)
    NATIVE_MILLIS_TIME = 3,   // millisTime()           -> Int (wall-clock ms epoch; Plain)
    NATIVE_ARGS        = 4,   // args()                 -> Array[String] (Plain)
    NATIVE_GET_ENV     = 5,   // getEnv(name)           -> String | nil  (wrapped Some/None)
    NATIVE_FILE_EXISTS = 6,   // fileExists(path)       -> Bool (Plain)
    NATIVE_DELETE_FILE = 7,   // deleteFile(path)       -> nil | String  (wrapped Ok/Err)
    NATIVE_LIST_DIR    = 8,   // listDir(path)          -> Array[String] | String (wrapped Ok/Err)
    NATIVE_MAKE_DIR    = 9,   // mkdir(path)            -> nil | String  (wrapped Ok/Err)
    NATIVE_READ_LINE   = 10,  // readLine()             -> String | nil  (wrapped Some/None; None at EOF)
    NATIVE_READ_ALL_STDIN = 11, // readAllStdin()       -> String (Plain; "" at EOF)
    NATIVE_RUN_PROCESS = 12,  // rawRun(argv, input)    -> [stdoutBytes, stderrBytes, exitInt] | String (wrapped Ok/Err)
    NATIVE_PARSE_INT   = 13,  // parseInt(s)            -> Int    | String (wrapped Ok/Err; std::from_chars, base 10, 48-bit)
    NATIVE_PARSE_DOUBLE = 14, // parseDouble(s)         -> Double | String (wrapped Ok/Err; std::from_chars, general)
    NATIVE_RAW_GC_STATS = 15, // rawGcStats()           -> Array[Double] of 8 GcStats counters (Plain; introspection)
    NATIVE_GC_RESET_STATS = 16, // gcResetStats()       -> nil (Plain; clears the GcStats counters)
    NATIVE_APPEND_FILE = 17,  // appendFile(path, bytes) -> nil | String  (wrapped Ok/Err; opens in append mode)
    NATIVE_IS_FILE     = 18,  // isFile(path)           -> Bool (Plain; is_regular_file)
    NATIVE_IS_DIR      = 19,  // isDir(path)            -> Bool (Plain; is_directory)
    NATIVE_FILE_SIZE   = 20,  // fileSize(path)         -> Int  | String (wrapped Ok/Err; immediate Int payload)
    NATIVE_RENAME      = 21,  // rename(from, to)       -> nil | String  (wrapped Ok/Err; overwrites same-fs)
    NATIVE_COPY_FILE   = 22,  // copyFile(from, to)     -> nil | String  (wrapped Ok/Err; existing dst => Err)
    // std::math libm natives (Plain; Double->Double unless noted; IEEE semantics -- domain errors -> NaN/inf).
    NATIVE_SQRT        = 23,  // sqrt(x)
    NATIVE_CBRT        = 24,  // cbrt(x)
    NATIVE_POW         = 25,  // pow(x, y)              (2-arg)
    NATIVE_HYPOT       = 26,  // hypot(x, y)            (2-arg)
    NATIVE_EXP         = 27,  // exp(x)
    NATIVE_LN          = 28,  // ln(x)   -- natural log
    NATIVE_LOG2        = 29,  // log2(x)
    NATIVE_LOG10       = 30,  // log10(x)
    NATIVE_SIN         = 31,  // sin(x)
    NATIVE_COS         = 32,  // cos(x)
    NATIVE_TAN         = 33,  // tan(x)
    NATIVE_ASIN        = 34,  // asin(x)
    NATIVE_ACOS        = 35,  // acos(x)
    NATIVE_ATAN        = 36,  // atan(x)
    NATIVE_ATAN2       = 37,  // atan2(y, x)            (2-arg)
    NATIVE_FABS        = 38,  // abs(x)  -- std::fabs (correct on -0.0/NaN)
    NATIVE_ISNAN       = 39,  // isNaN(x)      -> Bool
    NATIVE_ISINF       = 40,  // isInfinite(x) -> Bool
    NATIVE_F64_TO_BYTES = 41, // f64ToBytes(x) -> Bytes (8 raw IEEE-754 bytes, little-endian; Plain)
    // TCP networking natives -- std::net (opt-in). Blocking sockets; a socket is a small Int
    // DESCRIPTOR (an index into the per-VM socket registry, never a raw OS handle). Success = an
    // Int descriptor / Bytes / nil; failure = a String error message (wrapped Ok/Err by the compiler).
    NATIVE_TCP_CONNECT  = 42, // tcpConnect(host, port)  -> Int    | String (descriptor; getaddrinfo v4/v6, connect timeout)
    NATIVE_TCP_SEND     = 43, // tcpSend(sock, bytes)    -> nil    | String (send-all)
    NATIVE_TCP_RECV     = 44, // tcpRecv(sock, maxBytes) -> Bytes  | String (empty Bytes = EOF)
    NATIVE_TCP_CLOSE    = 45, // tcpClose(sock)          -> nil    | String (closesocket; frees the slot)
    NATIVE_TCP_LISTEN   = 46, // tcpListen(port)         -> Int    | String (dual-stack v6 bind+listen)
    NATIVE_TCP_ACCEPT   = 47, // tcpAccept(sock)         -> Int    | String (blocks; new descriptor)
    NATIVE_TCP_SET_TIMEOUT = 48, // tcpSetTimeout(sock, ms) -> nil | String (SO_RCVTIMEO/SO_SNDTIMEO; 0 = block)
    NATIVE_SHA256      = 49,  // sha256(data)           -> Bytes (32-byte FIPS 180-4 digest; Plain, total)
    // Appended (NOT slotted in beside the other TCP ids): a NativeId is baked into emitted bytecode,
    // so this enum is APPEND-ONLY -- renumbering `sha256` to keep the TCP block contiguous would
    // silently repoint every existing `.skbc`.
    NATIVE_TCP_LOCAL_PORT = 50, // tcpLocalPort(sock)   -> Int    | String (getsockname; the port a
                                //   listener actually bound -- the point of `tcpListen(0)`, which asks
                                //   the OS for a free one instead of guessing a fixed number)
    NATIVE_OS_ID       = 51,    // rawOsId()              -> Int (0 Windows, 1 macOS, 2 other; Plain, total)
    // std::poll -- non-blocking I/O + readiness. "Would block" is neither a value nor an error, so
    // each of these encodes it in the SUCCESS channel and the prelude turns it into an enum arm.
    NATIVE_SET_NON_BLOCKING = 52, // rawSetNonBlocking(sock, on) -> nil | String
    NATIVE_POLL        = 53,    // rawPoll(fds, interest, timeoutMs) -> Array[Int] | String (flags
                                //   index-parallel to fds; 1 readable, 2 writable, 4 closed/error)
    NATIVE_ACCEPT_NB   = 54,    // rawAcceptNb(sock)      -> Int    | String (>= 0 descriptor, -1 would block)
    NATIVE_RECV_NB     = 55,    // rawRecvNb(sock, max)   -> Array[Bytes] | String (0 elements = would
                                //   block; 1 element = the read, EMPTY = peer closed)
    NATIVE_SEND_NB     = 56,    // rawSendNb(sock, data)  -> Int    | String (bytes ACCEPTED, may be
                                //   short; 0 = would block -- there is no send-all on a nb socket)
    // Fork-join tasks (concurrency stage 2). A task runs a top-level function on its own thread,
    // with its own heap, over the same program image; its argument and result are COPIED
    // (vmcore/ValueCodec.h). A task is exposed as a small Int id into the per-execute() registry.
    NATIVE_TASK_SPAWN  = 57,    // rawSpawn(fn, arg)      -> Int  (the task id; Plain -- a refused
                                //   argument or a failed thread start raises a located fault)
    NATIVE_TASK_JOIN   = 58,    // rawJoin(id)            -> Bool (waits; true = the task returned)
    NATIVE_TASK_TAKE   = 59,    // rawTaskTake(id)        -> the result, or the failure message String
                                //   (after rawJoin; which one is rawJoin's Bool, NOT the kind)
    NATIVE_TASK_INPUT  = 60,    // rawTaskInput()         -> the task's argument (only the entry stub
                                //   execute() appends for a task calls it)
    // Actors (concurrency stage 2): long-lived isolates with a mailbox, in the same world-wide runtime
    // as tasks. Ids are world-wide, so an actor address can travel inside a message.
    NATIVE_ACTOR_SPAWN = 61,    // rawSpawnActor(fn, arg)  -> Int  (the actor id)
    NATIVE_SEND        = 62,    // rawSend(pid, msg)       -> Bool (false: the addressee no longer runs)
    NATIVE_RECEIVE     = 63,    // rawReceive(inbox, ms)   -> Int  (0 timeout, 1 message, 2 exit report, 3 stop)
    NATIVE_MAIL_MSG    = 64,    // rawMailMsg(inbox)       -> the message rawReceive took
    NATIVE_MAIL_FROM   = 65,    // rawMailFrom(inbox)      -> Int, the actor an exit report is about
    NATIVE_MAIL_REASON = 66,    // rawMailReason(inbox)    -> String, its fault message
    NATIVE_MAIN_INBOX  = 67,    // rawMainInbox()          -> Int  (0: the main program's mailbox; once)
    NATIVE_SELF_ID     = 68,    // rawSelfId()             -> Int  (this isolate's id; 0 = the root)
    // Handing a connection to another isolate (std::net, stage B of the actor work). The socket moves
    // between NetRegistries through a world-wide ticket; the sender's descriptor goes stale.
    NATIVE_HAND_OFF    = 69,    // rawHandOff(sock)        -> Int    | String (a ticket)
    NATIVE_TAKE        = 70,    // rawTake(ticket)         -> Int    | String (a descriptor in THIS isolate; once)
    NATIVE_COUNT       = 71,
};

// How the COMPILER lowers a native's heap-kind result into a surface value.
// Result / Option are wrapped (Ok/Err resp. Some/None); Plain is the bare value.
// See "Native functions" in docs/VirtualMachine.md.
enum NativeReturn : uint8_t { NRET_RESULT, NRET_OPTION, NRET_PLAIN };

// Maps a surface builtin name to its native id, or -1 if the name is not a
// native. The compiler uses this to lower a native call. Names are camelCase,
// consistent with toString / toBytes / fromBytes / toVec.
inline int native_id_of(const std::string& name) {
    if (name == "readFile")   return NATIVE_READ_FILE;
    if (name == "writeFile")  return NATIVE_WRITE_FILE;
    if (name == "nanoTime")   return NATIVE_NANO_TIME;
    if (name == "millisTime") return NATIVE_MILLIS_TIME;
    if (name == "args")       return NATIVE_ARGS;
    if (name == "getEnv")     return NATIVE_GET_ENV;
    if (name == "fileExists") return NATIVE_FILE_EXISTS;
    if (name == "deleteFile") return NATIVE_DELETE_FILE;
    if (name == "listDir")    return NATIVE_LIST_DIR;
    if (name == "mkdir")      return NATIVE_MAKE_DIR;
    if (name == "readLine")   return NATIVE_READ_LINE;
    if (name == "readAllStdin") return NATIVE_READ_ALL_STDIN;
    if (name == "rawRun")     return NATIVE_RUN_PROCESS;
    if (name == "parseInt")   return NATIVE_PARSE_INT;
    if (name == "parseDouble") return NATIVE_PARSE_DOUBLE;
    if (name == "rawGcStats") return NATIVE_RAW_GC_STATS;
    if (name == "gcResetStats") return NATIVE_GC_RESET_STATS;
    if (name == "appendFile") return NATIVE_APPEND_FILE;
    if (name == "isFile")     return NATIVE_IS_FILE;
    if (name == "isDir")      return NATIVE_IS_DIR;
    if (name == "fileSize")   return NATIVE_FILE_SIZE;
    if (name == "rename")     return NATIVE_RENAME;
    if (name == "copyFile")   return NATIVE_COPY_FILE;
    if (name == "sqrt")       return NATIVE_SQRT;
    if (name == "cbrt")       return NATIVE_CBRT;
    if (name == "pow")        return NATIVE_POW;
    if (name == "hypot")      return NATIVE_HYPOT;
    if (name == "exp")        return NATIVE_EXP;
    if (name == "ln")         return NATIVE_LN;
    if (name == "log2")       return NATIVE_LOG2;
    if (name == "log10")      return NATIVE_LOG10;
    if (name == "sin")        return NATIVE_SIN;
    if (name == "cos")        return NATIVE_COS;
    if (name == "tan")        return NATIVE_TAN;
    if (name == "asin")       return NATIVE_ASIN;
    if (name == "acos")       return NATIVE_ACOS;
    if (name == "atan")       return NATIVE_ATAN;
    if (name == "atan2")      return NATIVE_ATAN2;
    if (name == "abs")        return NATIVE_FABS;
    if (name == "isNaN")      return NATIVE_ISNAN;
    if (name == "isInfinite") return NATIVE_ISINF;
    if (name == "f64ToBytes") return NATIVE_F64_TO_BYTES;
    if (name == "tcpConnect") return NATIVE_TCP_CONNECT;
    if (name == "tcpSend")    return NATIVE_TCP_SEND;
    if (name == "tcpRecv")    return NATIVE_TCP_RECV;
    if (name == "tcpClose")   return NATIVE_TCP_CLOSE;
    if (name == "tcpListen")  return NATIVE_TCP_LISTEN;
    if (name == "tcpAccept")  return NATIVE_TCP_ACCEPT;
    if (name == "tcpLocalPort") return NATIVE_TCP_LOCAL_PORT;
    if (name == "tcpSetTimeout") return NATIVE_TCP_SET_TIMEOUT;
    if (name == "sha256")     return NATIVE_SHA256;
    if (name == "rawOsId")    return NATIVE_OS_ID;
    if (name == "rawSetNonBlocking") return NATIVE_SET_NON_BLOCKING;
    if (name == "rawPoll")    return NATIVE_POLL;
    if (name == "rawAcceptNb") return NATIVE_ACCEPT_NB;
    if (name == "rawRecvNb")  return NATIVE_RECV_NB;
    if (name == "rawSendNb")  return NATIVE_SEND_NB;
    if (name == "rawSpawn")   return NATIVE_TASK_SPAWN;
    if (name == "rawJoin")    return NATIVE_TASK_JOIN;
    if (name == "rawTaskTake") return NATIVE_TASK_TAKE;
    if (name == "rawTaskError") return NATIVE_TASK_TAKE;   // the same native, typed as the failure message
    if (name == "rawTaskInput") return NATIVE_TASK_INPUT;
    if (name == "rawSpawnActor") return NATIVE_ACTOR_SPAWN;
    if (name == "rawSend")    return NATIVE_SEND;
    if (name == "rawReceive") return NATIVE_RECEIVE;
    if (name == "rawMailMsg") return NATIVE_MAIL_MSG;
    if (name == "rawMailFrom") return NATIVE_MAIL_FROM;
    if (name == "rawMailReason") return NATIVE_MAIL_REASON;
    if (name == "rawMainInbox") return NATIVE_MAIN_INBOX;
    if (name == "rawSelfId")  return NATIVE_SELF_ID;
    if (name == "rawHandOff") return NATIVE_HAND_OFF;
    if (name == "rawTake")    return NATIVE_TAKE;
    return -1;
}

// The return category of a native id -- drives compile_native_call's lowering.
inline NativeReturn native_return_of(int id) {
    switch (id) {
        case NATIVE_READ_FILE:
        case NATIVE_WRITE_FILE:
        case NATIVE_DELETE_FILE:
        case NATIVE_LIST_DIR:
        case NATIVE_MAKE_DIR:
        case NATIVE_PARSE_INT:
        case NATIVE_PARSE_DOUBLE:
        case NATIVE_APPEND_FILE:
        case NATIVE_FILE_SIZE:
        case NATIVE_RENAME:
        case NATIVE_COPY_FILE:
        case NATIVE_TCP_CONNECT:
        case NATIVE_TCP_SEND:
        case NATIVE_TCP_RECV:
        case NATIVE_TCP_CLOSE:
        case NATIVE_TCP_LISTEN:
        case NATIVE_TCP_ACCEPT:
        case NATIVE_TCP_SET_TIMEOUT:
        case NATIVE_TCP_LOCAL_PORT:
        case NATIVE_SET_NON_BLOCKING:
        case NATIVE_POLL:
        case NATIVE_ACCEPT_NB:
        case NATIVE_RECV_NB:
        case NATIVE_SEND_NB:
        case NATIVE_RUN_PROCESS: return NRET_RESULT;
        case NATIVE_GET_ENV:
        case NATIVE_READ_LINE:   return NRET_OPTION;
        case NATIVE_NANO_TIME:
        case NATIVE_MILLIS_TIME:
        case NATIVE_ARGS:
        case NATIVE_FILE_EXISTS:
        case NATIVE_IS_FILE:
        case NATIVE_IS_DIR:
        case NATIVE_RAW_GC_STATS:
        case NATIVE_GC_RESET_STATS:
        case NATIVE_SQRT:  case NATIVE_CBRT:  case NATIVE_POW:   case NATIVE_HYPOT:
        case NATIVE_EXP:   case NATIVE_LN:    case NATIVE_LOG2:  case NATIVE_LOG10:
        case NATIVE_SIN:   case NATIVE_COS:   case NATIVE_TAN:   case NATIVE_ASIN:
        case NATIVE_ACOS:  case NATIVE_ATAN:  case NATIVE_ATAN2: case NATIVE_FABS:
        case NATIVE_ISNAN: case NATIVE_ISINF:
        case NATIVE_F64_TO_BYTES:
        case NATIVE_SHA256:
        case NATIVE_OS_ID:
        case NATIVE_TASK_SPAWN:
        case NATIVE_TASK_JOIN:
        case NATIVE_TASK_TAKE:
        case NATIVE_TASK_INPUT:
        case NATIVE_ACTOR_SPAWN: case NATIVE_SEND: case NATIVE_RECEIVE: case NATIVE_MAIL_MSG:
        case NATIVE_MAIL_FROM: case NATIVE_MAIL_REASON: case NATIVE_MAIN_INBOX: case NATIVE_SELF_ID:
        case NATIVE_READ_ALL_STDIN: return NRET_PLAIN;
        default:                 return NRET_RESULT;
    }
}

// The surface arity of a native id (the compiler checks the call site against it).
inline int native_arity(int id) {
    switch (id) {
        case NATIVE_WRITE_FILE:
        case NATIVE_APPEND_FILE:
        case NATIVE_RENAME:
        case NATIVE_COPY_FILE:
        case NATIVE_POW:
        case NATIVE_HYPOT:
        case NATIVE_ATAN2:
        case NATIVE_TCP_CONNECT:
        case NATIVE_TCP_SEND:
        case NATIVE_TCP_RECV:
        case NATIVE_TCP_SET_TIMEOUT:
        case NATIVE_SET_NON_BLOCKING:
        case NATIVE_RECV_NB:
        case NATIVE_SEND_NB:
        case NATIVE_TASK_SPAWN:
        case NATIVE_ACTOR_SPAWN:
        case NATIVE_SEND:
        case NATIVE_RECEIVE:
        case NATIVE_RUN_PROCESS: return 2;
        case NATIVE_POLL:        return 3;
        case NATIVE_READ_FILE:
        case NATIVE_GET_ENV:
        case NATIVE_FILE_EXISTS:
        case NATIVE_DELETE_FILE:
        case NATIVE_LIST_DIR:
        case NATIVE_MAKE_DIR:
        case NATIVE_PARSE_INT:
        case NATIVE_PARSE_DOUBLE:
        case NATIVE_IS_FILE:
        case NATIVE_IS_DIR:
        case NATIVE_FILE_SIZE:
        case NATIVE_SQRT:  case NATIVE_CBRT:  case NATIVE_EXP:  case NATIVE_LN:
        case NATIVE_LOG2:  case NATIVE_LOG10: case NATIVE_SIN:  case NATIVE_COS:
        case NATIVE_TAN:   case NATIVE_ASIN:  case NATIVE_ACOS: case NATIVE_ATAN:
        case NATIVE_FABS:  case NATIVE_ISNAN: case NATIVE_ISINF:
        case NATIVE_TCP_CLOSE:
        case NATIVE_TCP_LISTEN:
        case NATIVE_TCP_ACCEPT:
        case NATIVE_TCP_LOCAL_PORT:
        case NATIVE_ACCEPT_NB:
        case NATIVE_TASK_JOIN:
        case NATIVE_TASK_TAKE:
        case NATIVE_MAIL_MSG:
        case NATIVE_MAIL_FROM:
        case NATIVE_MAIL_REASON:
        case NATIVE_HAND_OFF:
        case NATIVE_TAKE:
        case NATIVE_SHA256:
        case NATIVE_F64_TO_BYTES: return 1;
        case NATIVE_NANO_TIME:
        case NATIVE_MILLIS_TIME:
        case NATIVE_ARGS:
        case NATIVE_READ_LINE:
        case NATIVE_RAW_GC_STATS:
        case NATIVE_GC_RESET_STATS:
        case NATIVE_TASK_INPUT:
        case NATIVE_MAIN_INBOX:
        case NATIVE_SELF_ID:
        case NATIVE_OS_ID:
        case NATIVE_READ_ALL_STDIN: return 0;
        default:                return 0;
    }
}
