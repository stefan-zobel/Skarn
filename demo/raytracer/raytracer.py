"""A faithful CPython port of this renderer -- the other half of the cross-language comparison in
README.md, and the only self-checking part of it.

    python demo/raytracer/raytracer.py [--preview]

FIDELITY IS THE POINT, AND IT IS CHECKABLE -- which is what makes this comparison worth more than the
loop microbenchmarks in demo/bench/bench.py. The Skarn renderer is deterministic (one seed, one random
stream per row, the same image on any number of threads) and pins its output's CRC-32. This port
reproduces the same generator (xoshiro128** with the same SplitMix32 seeding, row y seeded with SEED + y),
draws random numbers in the same ORDER within each row, and writes the same BMP, then checks the CRC
itself and exits non-zero on a mismatch. It renders sequentially. A matching CRC proves the
two programs performed the SAME computation, so the timings compare the languages rather than two
different pictures. Nothing else here is guarded, so if it drifts it will say so.

Two traps worth naming, because neither is visible in the timings:
  * `a / b` and `a * (1.0 / b)` are NOT bit-identical. The Skarn source divides in the sample loop,
    so this one divides too; "optimising" that away silently moves the image.
  * the camera parameters have to be READ, not assumed -- this scene is a 90 degree field of view
    from the origin, not the Ray-Tracing-in-One-Weekend default.

Style: classes with __slots__, which is both the closest analogue of Skarn's fixed-shape structs and
what a Python programmer would actually write for numeric code. No numpy: vectorising over pixels
would be a different program (and would move the work into C, the separate question demo/bench/bench.py
already asks).
"""
import math
import sys
import time
import zlib

# ---------------------------------------------------------------- configuration (main.skn)

WIDTH = 470
HEIGHT = 264
SAMPLES = 16
MAX_DEPTH = 12
SEED = 20260728
T_MAX = 1000000000.0

CRC_PREVIEW = 3886897764
CRC_FULL = 3340800133

# ---------------------------------------------------------------- Rng (std/random.skn)

M32 = 0xFFFFFFFF


def _rotl32(x, k):
    return ((x << k) | (x >> (32 - k))) & M32


def _sm_mix(z):
    z1 = ((z ^ (z >> 16)) * 0x21F0AAAD) & M32
    z2 = ((z1 ^ (z1 >> 15)) * 0x735A2D97) & M32
    return z2 ^ (z2 >> 15)


class Rng(object):
    __slots__ = ('s0', 's1', 's2', 's3')

    def __init__(self, a, b, c, d):
        x0 = a & M32
        x1 = b & M32
        x2 = c & M32
        x3 = d & M32
        self.s0 = x0 if (x0 | x1 | x2 | x3) != 0 else 1
        self.s1 = x1
        self.s2 = x2
        self.s3 = x3

    @staticmethod
    def from_seed(seed):
        z0 = (seed ^ (seed >> 24)) & M32
        z1 = (z0 + 0x9E3779B9) & M32
        a = _sm_mix(z1)
        z2 = (z1 + 0x9E3779B9) & M32
        b = _sm_mix(z2)
        z3 = (z2 + 0x9E3779B9) & M32
        c = _sm_mix(z3)
        z4 = (z3 + 0x9E3779B9) & M32
        d = _sm_mix(z4)
        return Rng(a, b, c, d)

    def next_u32(self):
        s0 = self.s0
        s1 = self.s1
        s2 = self.s2
        s3 = self.s3
        result = (_rotl32((s1 * 5) & M32, 7) * 9) & M32
        t = (s1 << 9) & M32
        n2 = s2 ^ s0
        n3 = s3 ^ s1
        self.s0 = s0 ^ n3
        self.s1 = s1 ^ n2
        self.s2 = n2 ^ t
        self.s3 = _rotl32(n3, 11)
        return result

    def next_double(self):
        hi = self.next_u32()
        lo = self.next_u32() & 0x1FFFFF
        return (hi * 2097152.0 + lo) * (1.0 / 9007199254740992.0)

    def next_double_range(self, lo, hi):
        return lo + (hi - lo) * self.next_double()


# ---------------------------------------------------------------- Vec3 (vec3.skn)

class Vec3(object):
    __slots__ = ('x', 'y', 'z')

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def add(self, b):
        return Vec3(self.x + b.x, self.y + b.y, self.z + b.z)

    def sub(self, b):
        return Vec3(self.x - b.x, self.y - b.y, self.z - b.z)

    def mul(self, b):
        return Vec3(self.x * b.x, self.y * b.y, self.z * b.z)

    def scale(self, k):
        return Vec3(self.x * k, self.y * k, self.z * k)

    def neg(self):
        return Vec3(-self.x, -self.y, -self.z)

    def dot(self, b):
        return self.x * b.x + self.y * b.y + self.z * b.z

    def cross(self, b):
        return Vec3(self.y * b.z - self.z * b.y,
                    self.z * b.x - self.x * b.z,
                    self.x * b.y - self.y * b.x)

    def length_squared(self):
        return self.x * self.x + self.y * self.y + self.z * self.z

    def length(self):
        return math.sqrt(self.length_squared())

    def unit(self):
        return self.scale(1.0 / self.length())

    def reflect(self, n):
        return self.sub(n.scale(2.0 * self.dot(n)))

    def near_zero(self):
        eps = 0.00000001
        return abs(self.x) < eps and abs(self.y) < eps and abs(self.z) < eps


def zero3():
    return Vec3(0.0, 0.0, 0.0)


# ---------------------------------------------------------------- Ray (ray.skn)

class Ray(object):
    __slots__ = ('origin', 'dir')

    def __init__(self, origin, d):
        self.origin = origin
        self.dir = d

    def at(self, t):
        return self.origin.add(self.dir.scale(t))


# ---------------------------------------------------------------- materials (material.skn)

class Scatter(object):
    __slots__ = ('ray', 'attenuation')

    def __init__(self, r, attenuation):
        self.ray = r
        self.attenuation = attenuation


def random_in_unit_sphere(rng):
    while True:
        p = Vec3(rng.next_double_range(-1.0, 1.0),
                 rng.next_double_range(-1.0, 1.0),
                 rng.next_double_range(-1.0, 1.0))
        if p.length_squared() < 1.0:
            return p


def random_unit_vector(rng):
    return random_in_unit_sphere(rng).unit()


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


class Lambertian(object):
    __slots__ = ('albedo',)

    def __init__(self, albedo):
        self.albedo = albedo

    def scatter(self, r_in, p, normal, rng):
        candidate = normal.add(random_unit_vector(rng))
        d = normal if candidate.near_zero() else candidate
        return Scatter(Ray(p, d), self.albedo)


class Metal(object):
    __slots__ = ('albedo', 'fuzz')

    def __init__(self, albedo, fuzz):
        self.albedo = albedo
        self.fuzz = clamp(fuzz, 0.0, 1.0)

    def scatter(self, r_in, p, normal, rng):
        reflected = r_in.dir.unit().reflect(normal)
        d = reflected.add(random_in_unit_sphere(rng).scale(self.fuzz))
        if d.dot(normal) > 0.0:
            return Scatter(Ray(p, d), self.albedo)
        return None


# ---------------------------------------------------------------- geometry (hittable.skn)

class Hit(object):
    __slots__ = ('t', 'p', 'normal', 'front_face', 'mat')

    def __init__(self, t, p, normal, front_face, mat):
        self.t = t
        self.p = p
        self.normal = normal
        self.front_face = front_face
        self.mat = mat


def face_normal(t, p, outward, r, mat):
    front = r.dir.dot(outward) < 0.0
    return Hit(t, p, outward if front else outward.neg(), front, mat)


class Sphere(object):
    __slots__ = ('center', 'radius', 'mat')

    def __init__(self, center, radius, mat):
        self.center = center
        self.radius = radius
        self.mat = mat

    def hit(self, r, t_min, t_max):
        oc = r.origin.sub(self.center)
        a = r.dir.length_squared()
        half_b = oc.dot(r.dir)
        c = oc.length_squared() - self.radius * self.radius
        disc = half_b * half_b - a * c
        if disc < 0.0:
            return None

        sqrtd = math.sqrt(disc)
        root = (-half_b - sqrtd) / a
        if root < t_min or t_max < root:
            root = (-half_b + sqrtd) / a
            if root < t_min or t_max < root:
                return None

        p = r.at(root)
        outward = p.sub(self.center).scale(1.0 / self.radius)
        return face_normal(root, p, outward, r, self.mat)


def hit_scene(world, r, t_min, t_max):
    best = None
    closest = t_max
    for obj in world:
        h = obj.hit(r, t_min, closest)
        if h is not None:
            closest = h.t
            best = h
    return best


# ---------------------------------------------------------------- camera (camera.skn)

class Camera(object):
    __slots__ = ('origin', 'lower_left', 'horizontal', 'vertical')

    def __init__(self, look_from, look_at, vup, vfov_degrees, aspect):
        h = math.tan(vfov_degrees * math.pi / 180.0 / 2.0)
        viewport_height = 2.0 * h
        viewport_width = aspect * viewport_height

        w = look_from.sub(look_at).unit()
        u = vup.cross(w).unit()
        v = w.cross(u)

        horizontal = u.scale(viewport_width)
        vertical = v.scale(viewport_height)
        self.origin = look_from
        self.lower_left = look_from.sub(horizontal.scale(0.5)).sub(vertical.scale(0.5)).sub(w)
        self.horizontal = horizontal
        self.vertical = vertical

    def ray_at(self, s, t):
        target = self.lower_left.add(self.horizontal.scale(s)).add(self.vertical.scale(t))
        return Ray(self.origin, target.sub(self.origin))


# ---------------------------------------------------------------- canvas + BMP (bmp.skn)

class Canvas(object):
    __slots__ = ('width', 'height', 'px')

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.px = [0] * (width * height)

    def set(self, x, y, r, g, b):
        self.px[y * self.width + x] = ((r & 255) << 16) | ((g & 255) << 8) | (b & 255)

    def to_bmp(self):
        row_bytes = self.width * 3
        pad = (4 - (row_bytes % 4)) % 4
        stride = row_bytes + pad
        data_size = stride * self.height
        out = bytearray()

        def u16(v):
            out.extend(v.to_bytes(2, 'little'))

        def u32(v):
            out.extend((v & 0xFFFFFFFF).to_bytes(4, 'little'))

        out.append(66)                 # 'B'
        out.append(77)                 # 'M'
        u32(54 + data_size)
        u32(0)
        u32(54)

        u32(40)
        u32(self.width)
        u32(self.height)               # positive => bottom-up
        u16(1)
        u16(24)
        u32(0)
        u32(data_size)
        u32(2835)
        u32(2835)
        u32(0)
        u32(0)

        for y in range(self.height - 1, -1, -1):
            row = y * self.width
            for x in range(self.width):
                p = self.px[row + x]
                out.append(p & 255)
                out.append((p >> 8) & 255)
                out.append((p >> 16) & 255)
            for _ in range(pad):
                out.append(0)
        return bytes(out)


# ---------------------------------------------------------------- render (main.skn)

def to_byte_gamma(v):
    return int(255.999 * math.sqrt(clamp(v, 0.0, 1.0)))


def ray_color(r, world, depth, rng):
    if depth <= 0:
        return zero3()
    h = hit_scene(world, r, 0.001, T_MAX)
    if h is not None:
        s = h.mat.scatter(r, h.p, h.normal, rng)
        if s is not None:
            return s.attenuation.mul(ray_color(s.ray, world, depth - 1, rng))
        return zero3()
    t = 0.5 * (r.dir.unit().y + 1.0)
    return Vec3(1.0, 1.0, 1.0).scale(1.0 - t).add(Vec3(0.5, 0.7, 1.0).scale(t))


def render(width, height, samples, cam, world):
    img = Canvas(width, height)
    # DIVISION, not multiplication by a precomputed reciprocal: a/b and a*(1/b) are not bit-identical,
    # and the Skarn source divides. Getting this "optimisation" wrong would move the image.
    fw = float(width - 1)
    fh = float(height - 1)
    inv_s = 1.0 / float(samples)          # this one IS a reciprocal in the Skarn source
    for y in range(height):
        rng = Rng.from_seed(SEED + y)       # one stream per row, as main.skn -- the image must not
                                            # depend on how rows are split among tasks
        for x in range(width):
            acc = zero3()
            for _ in range(samples):
                u = (float(x) + rng.next_double()) / fw
                v = 1.0 - (float(y) + rng.next_double()) / fh
                acc = acc.add(ray_color(cam.ray_at(u, v), world, MAX_DEPTH, rng))
            c = acc.scale(inv_s)
            img.set(x, y, to_byte_gamma(c.x), to_byte_gamma(c.y), to_byte_gamma(c.z))
    return img


def build_scene():
    ground = Lambertian(Vec3(0.8, 0.8, 0.0))
    matte = Lambertian(Vec3(0.1, 0.2, 0.5))
    mirror = Metal(Vec3(0.8, 0.8, 0.8), 0.05)
    gold = Metal(Vec3(0.8, 0.6, 0.2), 0.40)
    return [Sphere(Vec3(0.0, -100.5, -1.0), 100.0, ground),
            Sphere(Vec3(0.0, 0.0, -1.0), 0.5, matte),
            Sphere(Vec3(-1.0, 0.0, -1.0), 0.5, mirror),
            Sphere(Vec3(1.0, 0.0, -1.0), 0.5, gold)]


def main():
    preview = '--preview' in sys.argv
    width = 160 if preview else WIDTH
    height = 90 if preview else HEIGHT
    samples = 4 if preview else SAMPLES

    sys.setrecursionlimit(10000)
    world = build_scene()
    cam = Camera(zero3(), Vec3(0.0, 0.0, -1.0), Vec3(0.0, 1.0, 0.0),
                 90.0, float(width) / float(height))

    t0 = time.perf_counter()
    img = render(width, height, samples, cam, world)
    elapsed = time.perf_counter() - t0

    data = img.to_bmp()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    expected = CRC_PREVIEW if preview else CRC_FULL
    rays = width * height * samples

    print("py     %dx%d, %d samples/pixel, depth %d  (%d primary rays)"
          % (width, height, samples, MAX_DEPTH, rays))
    print("render wall_s=%.3f  us/ray=%.3f" % (elapsed, elapsed * 1e6 / rays))
    if crc == expected:
        print("image  crc32 %d -- MATCHES the Skarn image" % crc)
    else:
        print("image  crc32 %d -- MISMATCH, expected %d" % (crc, expected))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
