# The CPython counterpart of bench.skn -- the other half of the cross-language comparison described
# in that file's header. Three workloads, same sizes, each timing its own hot region exactly as the
# Skarn version does.
#
#   python demo/bench/bench.py
#
# Runs on CPython 3.6 through 3.11+ (see the timer shim below), so the comparison can be made against
# both a current CPython and an old one: a newer CPython only closes the gap where CPython itself
# changed.
#
# Workloads 1 and 3 are measured TWICE on purpose, because "how fast is this language" and "how fast
# is this program" are different questions:
#   a) the literal translation of the Skarn source -- same construct, same work, interpreter vs interpreter
#   b) what a Python programmer would actually write -- which routes the work into C
# Reporting only (a) flatters us; reporting only (b) flatters CPython. Both are given, and the spread
# between them is the reason a single "Skarn vs Python" ratio does not exist. For the honest whole-program number see demo/raytracer/README.md, which compares a real
# program and validates the comparison against a pinned CRC-32.
#
# NOTE on workload 1: Skarn's Int is 48-bit and WRAPS, so its checksum differs from CPython's exact
# value. Same work per iteration, different result -- not a fairness problem, but worth knowing.
import sys, time

# perf_counter_ns() only exists from 3.7 on; 3.6 needs the float-seconds counter. Same clock source.
try:
    _ns = time.perf_counter_ns
except AttributeError:
    def _ns():
        return int(time.perf_counter() * 1000000000)

n1 = 200000000

# 1a -- literal translation of the Skarn while-loop
t = _ns()
s = 0
i = 0
while i < n1:
    s = s + i
    i = i + 1
e = _ns() - t
print("arith_while  wall_ns=%d  ns/iter=%.2f" % (e, e / n1))

# 1b -- the idiomatic Python loop (for over range, still interpreted)
t = _ns()
s = 0
for i in range(n1):
    s += i
e = _ns() - t
print("arith_for    wall_ns=%d  ns/iter=%.2f" % (e, e / n1))

# 1c -- what a Python programmer really writes: the loop disappears into C
t = _ns()
s = sum(range(n1))
e = _ns() - t
print("arith_sum    wall_ns=%d  ns/iter=%.2f" % (e, e / n1))

# 2 -- naive recursive Fibonacci (no idiomatic C shortcut exists)
def fib(k):
    return k if k < 2 else fib(k - 1) + fib(k - 2)

sys.setrecursionlimit(10000)
t = _ns()
r = fib(36)
e = _ns() - t
print("fib          wall_ns=%d  fib=%d" % (e, r))

n3 = 5000000

# 3a -- literal translation: an explicit driver loop over the same filter/map/fold
t = _ns()
total = 0
for x in range(0, n3):
    if x % 2 == 0:
        total = total + (x + 1)
e = _ns() - t
print("iter_loop    wall_ns=%d  ns/elem=%.2f" % (e, e / n3))

# 3b -- idiomatic: the generator pipeline, driven by C
t = _ns()
total = sum(x + 1 for x in range(0, n3) if x % 2 == 0)
e = _ns() - t
print("iter_genexp  wall_ns=%d  ns/elem=%.2f" % (e, e / n3))
