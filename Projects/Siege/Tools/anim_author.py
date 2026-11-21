"""Animation clips the Universal Animation Libraries lack, authored on their skeleton: drawing and loosing a longbow,
holding, carrying and thrusting a spear, and riding a horse (sitting it, charging, cutting down at the infantry). Each clip starts from a library clip (the legs, the breathing, the other
arm) and poses the arms by two-bone IK toward targets placed in the character's space (it faces +Z, its left is +X),
with the hands turned so the weapon (rigid on the hand: build_soldiers.py) points where it should. A bow's string has a
joint of its own ("bowstring", under the left hand) that the draw pulls back to the right hand.

Used by build_animations.py (clips()); bowstring_local() is shared with build_soldiers.py."""
import os

import numpy as np

import gltfio as G
import siege as S

SRC = os.path.join(S.ASSETS, "Models", "Animations")
FPS = 30.0
# Where the bow is held, from the left hand in the bind pose (world meters; build_soldiers: the stave's middle), and
# how far the string's middle stands back from it (the bow's +Z, toward the archer).
BOW_HOLD = np.array([0.085, -0.02, 0.0])
STRING_BACK = 0.18
DRAW = 0.68  # from the grip to the anchor at full draw


# ---- rotations (matrices; glTF quaternions are x, y, z, w) ---------------------------------------------------------

def q2m(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def m2q(m):
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def slerp(a, b, t):
    a, b = np.asarray(a, float), np.asarray(b, float)
    d = float(np.dot(a, b))
    if d < 0:
        b, d = -b, -d
    if d > 0.9995:
        q = a + (b - a) * t
        return q / np.linalg.norm(q)
    th = np.arccos(d)
    return (np.sin((1 - t) * th) * a + np.sin(t * th) * b) / np.sin(th)


def axis_angle(axis, deg):
    axis = np.asarray(axis, float) / np.linalg.norm(axis)
    a = np.radians(deg)
    k = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + np.sin(a) * k + (1 - np.cos(a)) * k @ k


def between(a, b):
    """The smallest rotation taking direction a to direction b."""
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if c < -0.9999:
        perp = np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0])
        return axis_angle(perp, 180)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def orthonormal(m):
    u, _, vt = np.linalg.svd(m)
    return u @ vt


def frame(forward, up, forward_axis=(0, 0, 1), up_axis=(0, 1, 0)):
    """The rotation taking a mesh's forward_axis to `forward` and its up_axis to `up` (made perpendicular)."""
    f = np.asarray(forward, float) / np.linalg.norm(forward)
    u = np.asarray(up, float) - f * np.dot(up, f)
    u /= np.linalg.norm(u)
    fa, ua = np.asarray(forward_axis, float), np.asarray(up_axis, float)
    src = np.column_stack([fa, ua, np.cross(fa, ua)])
    dst = np.column_stack([f, u, np.cross(f, u)])
    return dst @ src.T


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


# ---- the skeleton and the library clips --------------------------------------------------------------------------

class Skeleton:
    def __init__(self):
        self.ual1 = G.Gltf(os.path.join(SRC, "UAL1_Standard.glb"))
        self.ual2 = G.Gltf(os.path.join(SRC, "UAL2_Standard.glb"))
        self.nodes = self.ual1.doc["nodes"]
        self.names = [n.get("name") for n in self.nodes]
        self.index = {n: i for i, n in enumerate(self.names) if n}
        self.parent = {}
        for i, n in enumerate(self.nodes):
            for c in n.get("children", []):
                self.parent[c] = i
        self.order = sorted(range(len(self.nodes)), key=self.depth)
        self.rest = [(np.array(n.get("translation", [0, 0, 0]), float), q2m(n.get("rotation", [0, 0, 0, 1])),
                      np.array(n.get("scale", [1, 1, 1]), float)) for n in self.nodes]
        self.clips = {}
        for g in (self.ual1, self.ual2):
            for a in g.doc["animations"]:
                self.clips.setdefault(a["name"], (g, a))
        self.bind = self.world(self.pose_rest())

    def depth(self, i):
        d = 0
        while i in self.parent:
            i, d = self.parent[i], d + 1
        return d

    def pose_rest(self):
        return [(t.copy(), r.copy(), s.copy()) for t, r, s in self.rest]

    def duration(self, clip):
        g, a = self.clips[clip]
        return max(float(g.read(s["input"]).max()) for s in a["samplers"])

    def sample(self, clip, t, loop=True):
        """The clip's pose at time t (looping or held): each joint's local translation, rotation, scale."""
        pose = self.pose_rest()
        g, a = self.clips[clip]
        length = self.duration(clip)
        t = t % length if loop and length > 0 else min(max(t, 0.0), length)
        for ch in a["channels"]:
            name = g.doc["nodes"][ch["target"]["node"]].get("name")
            if name not in self.index:
                continue
            s = a["samplers"][ch["sampler"]]
            times = g.read(s["input"]).astype(float).reshape(-1)
            values = g.read(s["output"]).astype(float).reshape(len(times), -1)
            k = int(np.clip(np.searchsorted(times, t) - 1, 0, len(times) - 1))
            k1 = min(k + 1, len(times) - 1)
            f = 0.0 if k1 == k else (t - times[k]) / (times[k1] - times[k])
            f = min(max(f, 0.0), 1.0)
            j = self.index[name]
            tr, rot, sc = pose[j]
            path = ch["target"]["path"]
            if path == "rotation":
                pose[j] = (tr, q2m(slerp(values[k], values[k1], f)), sc)
            elif path == "translation":
                pose[j] = (values[k] + (values[k1] - values[k]) * f, rot, sc)
            elif path == "scale":
                pose[j] = (tr, rot, values[k] + (values[k1] - values[k]) * f)
        return pose

    def world(self, pose):
        out = [None] * len(pose)
        for i in self.order:
            t, r, s = pose[i]
            m = np.eye(4)
            m[:3, :3] = r * s
            m[:3, 3] = t
            out[i] = out[self.parent[i]] @ m if i in self.parent else m
        return out

    # -- posing ------------------------------------------------------------------------------------------------------

    def set_world_rotation(self, pose, joint, rotation):
        j = self.index[joint]
        parent = self.world(pose)[self.parent[j]]
        t, _, s = pose[j]
        pose[j] = (t, orthonormal(parent[:3, :3]).T @ rotation, s)

    def turn(self, pose, joint, rotation):
        """Turns a joint (and so what hangs from it) by a world-space rotation."""
        w = self.world(pose)[self.index[joint]]
        self.set_world_rotation(pose, joint, rotation @ orthonormal(w[:3, :3]))

    def position(self, pose, joint):
        return self.world(pose)[self.index[joint]][:3, 3]

    def rotation(self, pose, joint):
        return orthonormal(self.world(pose)[self.index[joint]][:3, :3])

    def reach(self, pose, side, target, pole):
        """Two-bone IK: the upper arm and forearm of `side` ("l" or "r") turned so the hand reaches `target`, the
        elbow bending toward `pole` (a direction)."""
        self.limb(pose, f"upperarm_{side}", f"lowerarm_{side}", f"hand_{side}", target, pole)

    def step(self, pose, side, target, pole):
        """Two-bone IK for a leg: the ankle (foot joint) of `side` to `target`, the knee toward `pole`."""
        self.limb(pose, f"thigh_{side}", f"calf_{side}", f"foot_{side}", target, pole)

    def limb(self, pose, up, low, hand, target, pole):
        w = self.world(pose)
        s, e0, h0 = (w[self.index[n]][:3, 3] for n in (up, low, hand))
        l1, l2 = np.linalg.norm(e0 - s), np.linalg.norm(h0 - e0)
        v = np.asarray(target, float) - s
        d = min(np.linalg.norm(v), (l1 + l2) * 0.999)
        n = v / np.linalg.norm(v)
        a = (l1 * l1 - l2 * l2 + d * d) / (2 * d)
        h = np.sqrt(max(l1 * l1 - a * a, 0.0))
        p = np.asarray(pole, float) - n * np.dot(pole, n)
        p /= max(np.linalg.norm(p), 1e-9)
        elbow, wrist = s + n * a + p * h, s + n * d
        self.turn(pose, up, between(e0 - s, elbow - s))
        w = self.world(pose)
        e, h1 = w[self.index[low]][:3, 3], w[self.index[hand]][:3, 3]
        self.turn(pose, low, between(h1 - e, wrist - e))

    def shift(self, pose, joint, delta):
        """Moves a joint by a world-space offset."""
        j = self.index[joint]
        parent = self.world(pose)[self.parent[j]]
        t, r, s = pose[j]
        pose[j] = (t + np.linalg.inv(parent[:3, :3]) @ np.asarray(delta, float), r, s)

    def bind_rotation(self, joint):
        return orthonormal(self.bind[self.index[joint]][:3, :3])


def bowstring_local(hand_world):
    """The bowstring joint's place under the left hand (in its space): the middle of the string, held in the fist."""
    hold = hand_world[:3, 3] + BOW_HOLD + np.array([0.0, 0.0, STRING_BACK])
    return (np.linalg.inv(hand_world) @ np.append(hold, 1.0))[:3]


# ---- the clips --------------------------------------------------------------------------------------------------------

class Clip:
    """Keys for every joint (rotation, and translation where the base clip moves it), sampled at FPS."""

    def __init__(self, sk, name, length):
        self.sk, self.name, self.length = sk, name, length
        self.times = list(np.arange(0.0, length + 1e-6, 1.0 / FPS))
        self.rot = {}
        self.tr = {}

    def key(self, pose, moved=(), extra=None):
        for j, (t, r, s) in enumerate(pose):
            name = self.sk.names[j]
            if not name or name in ("Armature",):
                continue
            self.rot.setdefault(name, []).append(m2q(r))
            self.tr.setdefault(name, []).append(t.copy())
        for name, value in (extra or {}).items():
            self.tr.setdefault(name, []).append(np.asarray(value, float))

    def tracks(self, translated):
        out = {}
        for name, qs in self.rot.items():
            qs = np.array(qs)
            for i in range(1, len(qs)):  # the short way round between keys
                if np.dot(qs[i], qs[i - 1]) < 0:
                    qs[i] = -qs[i]
            out[name] = {"rotation": (self.times, qs)}
        for name in translated:
            if name in self.tr:
                out.setdefault(name, {})["translation"] = (self.times, np.array(self.tr[name]))
        return {"name": self.name, "tracks": out}


def moved_joints(sk, clip):
    g, a = sk.clips[clip]
    return {g.doc["nodes"][c["target"]["node"]].get("name") for c in a["channels"] if c["target"]["path"] == "translation"}


def bow_shoot(sk, name="Bow_Shoot", pitch=28.0):
    """Raise the bow level and nock (0-0.35 s); draw to the jaw while raising it to the aim, turned side-on (to 0.9);
    hold; loose at 1.2 (the string snaps forward, the draw hand flies back past the jaw); lower again (to 1.9)."""
    base, length, release = "Idle_Loop", 1.9, 1.2
    clip = Clip(sk, name, length)
    hand_l_bind = sk.bind_rotation("hand_l")
    rest_string = bowstring_local(sk.bind[sk.index["hand_l"]])
    up = np.array([0.0, 1.0, 0.0])

    def aim_at(deg):
        p = np.radians(deg)
        return np.array([0.0, np.sin(p), np.cos(p)])

    def bow_frame(aim):
        # The stave toward the target (the bow's -Z: from the string to the stave), canted a little to the right.
        return frame(aim, axis_angle(aim, -10) @ (up - aim * aim[1]), forward_axis=(0, 0, -1))

    for t in clip.times:
        pose = sk.sample(base, t)
        raise_w = smooth(t / 0.35) * (1 - smooth((t - 1.4) / 0.5))
        draw_k = smooth((t - 0.35) / 0.55) if t < release else 1.0
        # Side-on: the chest turns the left shoulder toward the target, the head back to look along the arrow.
        for joint in ("spine_01", "spine_02", "spine_03"):
            sk.turn(pose, joint, axis_angle(up, -17 * raise_w))
        sk.turn(pose, "Head", axis_angle(up, 46 * raise_w))
        hand_l0, hand_r0 = sk.position(pose, "hand_l"), sk.position(pose, "hand_r")
        rot_l0 = sk.rotation(pose, "hand_l")
        shoulder = sk.position(pose, "upperarm_l")
        head = sk.position(pose, "Head")
        aim = aim_at(5.0 + (pitch - 5.0) * draw_k)
        anchor = head + np.array([-0.04, -0.06, 0.09])  # the right side of the jaw
        set_grip = shoulder + np.array([0.0, -0.14, 0.45])  # nocking: the bow low and close
        grip = set_grip + (anchor + aim * DRAW - set_grip) * draw_k
        bow = bow_frame(aim)
        string_rest = grip - aim * STRING_BACK
        if t < release:
            string = string_rest + (anchor - string_rest) * draw_k
            hand_draw = string - aim * 0.05
        else:
            string = string_rest + (anchor - string_rest) * (1 - smooth((t - release) / 0.06))
            hand_draw = anchor - aim * (0.05 + 0.1 * smooth((t - release) / 0.12))
        hand_draw = hand_draw + np.array([0.0, 0.002, 0.0]) * np.sin(t * 40) * (0.9 < t < release)  # against the pull
        if raise_w > 0:
            hand_bow = grip - bow @ BOW_HOLD
            sk.reach(pose, "l", hand_l0 + (hand_bow - hand_l0) * raise_w, pole=[0.6, -1.0, -0.2])
            sk.set_world_rotation(pose, "hand_l", q2m(slerp(m2q(rot_l0), m2q(bow @ hand_l_bind), raise_w)))
            sk.reach(pose, "r", hand_r0 + (hand_draw - hand_r0) * raise_w, pole=[-0.4, 0.5, -1.0])
        # The string's middle, in the left hand's space (where the bow is): drawn back, or at rest in the bow.
        local = rest_string
        if raise_w > 0:
            hand_l = sk.world(pose)[sk.index["hand_l"]]
            drawn = (np.linalg.inv(hand_l) @ np.append(string, 1.0))[:3]
            local = rest_string + (drawn - rest_string) * min(raise_w * 4.0, 1.0)
        clip.key(pose, extra={"bowstring": local})
    return clip.tracks(moved_joints(sk, base) | {"bowstring"})


def spear_frame(sk, direction):
    """The right hand's world rotation that points the spear (rigid on it, forward in the bind pose) along `direction`."""
    up = np.array([0.0, 1.0, 0.0]) if abs(direction[1]) < 0.9 else np.array([0.0, 0.0, -1.0])
    return frame(direction, up) @ sk.bind_rotation("hand_r")


def hold_spear(sk, pose, offset, direction, weight=1.0, pole=(-1.0, -1.0, -0.4)):
    """The right hand at `offset` from the pelvis (character space), the spear along `direction`."""
    pelvis = sk.position(pose, "pelvis")
    hand0, rot0 = sk.position(pose, "hand_r"), sk.rotation(pose, "hand_r")
    target = pelvis + np.asarray(offset, float)
    sk.reach(pose, "r", hand0 + (target - hand0) * weight, pole=pole)
    want = spear_frame(sk, np.asarray(direction, float) / np.linalg.norm(direction))
    sk.set_world_rotation(pose, "hand_r", q2m(slerp(m2q(rot0), m2q(want), weight)))


def spear_loop(sk, name, base, offset, direction, pole=(-1.0, -1.0, -0.4)):
    length = sk.duration(base)
    clip = Clip(sk, name, length)
    for t in clip.times:
        pose = sk.sample(base, t)
        hold_spear(sk, pose, offset, direction, pole=pole)
        clip.key(pose)
    return clip.tracks(moved_joints(sk, base))


# The spear's ready hold: by the right hip, clear of the shield on the left arm, the point forward (a little in, to a
# foe straight ahead) and up.
READY = ((-0.34, 0.12, 0.22), (0.06, 0.22, 1.0))


def spear_thrust(sk, name, height=0.30, aim_down=0.06, lunge=0.14):
    """From the ready hold: drawn back (0-0.25 s), driven forward with a lunge (the point arrives at 0.42), held, and
    brought back to the ready hold (to 1.0)."""
    base, length = "Sword_Idle", 1.0
    clip = Clip(sk, name, length)
    ready_at, ready_dir = np.array(READY[0]), np.array(READY[1])
    back_at = ready_at + np.array([-0.02, 0.06, -0.22])
    out_at = np.array([-0.31, height, 0.66])
    out_dir = np.array([0.1, -aim_down, 1.0])
    up = np.array([0.0, 1.0, 0.0])
    for t in clip.times:
        pose = sk.sample(base, t)
        if t < 0.25:
            k = smooth(t / 0.25)
            at, d, lean = ready_at + (back_at - ready_at) * k, ready_dir, -0.3 * k
        elif t < 0.42:
            k = smooth((t - 0.25) / 0.17) ** 0.7
            at, d, lean = back_at + (out_at - back_at) * k, ready_dir + (out_dir - ready_dir) * k, -0.3 + 1.3 * k
        elif t < 0.55:
            at, d, lean = out_at, out_dir, 1.0
        else:
            k = smooth((t - 0.55) / 0.45)
            at, d, lean = out_at + (ready_at - out_at) * k, out_dir + (ready_dir - out_dir) * k, 1.0 - k
        # The lunge: the body forward over the front foot, the chest leaning in, the right shoulder driving.
        sk.shift(pose, "pelvis", np.array([0.0, -0.03, lunge]) * max(lean, 0.0))
        sk.turn(pose, "spine_02", axis_angle([1, 0, 0], 9 * lean))
        sk.turn(pose, "spine_03", axis_angle(up, -12 * lean))
        hold_spear(sk, pose, at, d)
        clip.key(pose)
    return clip.tracks(moved_joints(sk, base) | {"pelvis"})


# ---- riding ----------------------------------------------------------------------------------------------------------
# The rider's pelvis in its own space while mounted (Battle.cs's Mount seats the root so this lands on the saddle), and
# the stirrups (the ankles) either side of the horse's barrel, a little ahead of the hips; a knight's left hand holds
# the reins over the pommel with the shield on that forearm (its face out to the left), the sword in the right.
RIDE_PELVIS = np.array([0.0, 1.0, 0.0])
STIRRUP = np.array([0.37, 0.42, 0.16])  # x: either side (left +X), y: above the root, z: forward
REINS = np.array([0.10, 0.22, 0.34])     # from the pelvis


def seat(sk, pose, lean=0.0, rise=0.0, forward=0.0, turn=0.0, roll=0.0, look=1.0):
    """Sits the base pose on the horse: the pelvis on the saddle (`rise` up off it, `forward` over the pommel), the
    chest leaning forward by `lean` degrees, turned right by `turn` and over to the right by `roll`, the head kept
    level `look` (1: looking ahead), the feet in the stirrups, heels down, toes out."""
    up = np.array([0.0, 1.0, 0.0])
    sk.shift(pose, "pelvis", RIDE_PELVIS + np.array([0.0, rise, forward]) - sk.position(pose, "pelvis"))
    for joint, share in (("spine_01", 0.35), ("spine_02", 0.35), ("spine_03", 0.3)):
        sk.turn(pose, joint, axis_angle(up, -turn * share) @ axis_angle([0, 0, 1], -roll * share) @ axis_angle([1, 0, 0], lean * share))
    sk.turn(pose, "Head", axis_angle([1, 0, 0], -lean * look * 0.8) @ axis_angle(up, turn * look * 0.5))
    for side, sx in (("l", 1.0), ("r", -1.0)):
        ankle = STIRRUP * np.array([sx, 1.0, 1.0])
        sk.step(pose, side, ankle, pole=[sx * 0.55, 0.0, 1.0])
        foot = axis_angle(up, sx * 14) @ axis_angle([1, 0, 0], -10) @ sk.bind_rotation(f"foot_{side}")
        sk.set_world_rotation(pose, f"foot_{side}", foot)


def reins(sk, pose, offset=(0.0, 0.0, 0.0)):
    """The left hand on the reins over the pommel, the shield on its forearm facing out to the left."""
    target = sk.position(pose, "pelvis") + REINS + np.asarray(offset, float)
    sk.reach(pose, "l", target, pole=[1.0, -0.4, -0.6])
    out = np.array([0.9, 0.0, 0.4])
    sk.set_world_rotation(pose, "hand_l", frame(out / np.linalg.norm(out), [0.0, -1.0, 0.0]) @ sk.bind_rotation("hand_l"))


# The sword's holds, from the pelvis: upright at rest; raised forward for the charge.
RIDE_REST = ((-0.24, 0.18, 0.30), (0.05, 1.0, 0.3), (-1.0, -0.6, 0.6))
RIDE_CHARGE = ((-0.30, 0.40, 0.34), (0.12, 0.55, 1.0), (-1.0, -0.8, -0.4))


def ride_loop(sk, name, lean, rise, forward, sword):
    base = "Idle_Loop"  # (its breathing, in the chest and head)
    clip = Clip(sk, name, sk.duration(base))
    for t in clip.times:
        pose = sk.sample(base, t)
        seat(sk, pose, lean=lean, rise=rise, forward=forward)
        reins(sk, pose, offset=(0.0, 0.0, forward + 0.25 * np.sin(np.radians(lean))))
        hold_spear(sk, pose, sword[0], sword[1], pole=sword[2])
        clip.key(pose)
    return clip.tracks(moved_joints(sk, base) | {"pelvis"})


def ride_slash(sk, name="Ride_Slash"):
    """From the charge: the sword drawn up and back over the right shoulder (to 0.3 s), cut down past the horse's right
    shoulder at the man on foot, the body turning and leaning into it (the edge low at 0.48), held, and back up to the
    charge (to 1.1)."""
    base, length = "Idle_Loop", 1.1
    clip = Clip(sk, name, length)
    keys = [  # time, (hand from the pelvis, blade direction, elbow), lean, turn, roll
        (0.0, RIDE_CHARGE, 20.0, 0.0, 0.0),
        (0.3, ((-0.20, 0.72, -0.02), (-0.15, 0.5, -1.0), (-1.0, 0.4, -0.4)), 8.0, -18.0, -6.0),
        (0.48, ((-0.66, 0.02, 0.42), (-0.5, -0.75, 0.45), (-0.6, 0.6, -0.8)), 26.0, 22.0, 16.0),
        (0.6, ((-0.66, -0.04, 0.36), (-0.45, -0.8, 0.35), (-0.6, 0.6, -0.8)), 26.0, 22.0, 16.0),
        (1.1, RIDE_CHARGE, 20.0, 0.0, 0.0),
    ]
    for t in clip.times:
        pose = sk.sample(base, t)
        k = max(i for i in range(len(keys)) if keys[i][0] <= t + 1e-9)
        a = keys[min(k, len(keys) - 1)]
        b = keys[min(k + 1, len(keys) - 1)]
        f = 0.0 if b[0] == a[0] else smooth((t - a[0]) / (b[0] - a[0]))
        if k == 1:  # the cut: fast through, slow at the end
            f = f ** 0.7

        def mix(x, y):
            return np.asarray(x, float) + (np.asarray(y, float) - np.asarray(x, float)) * f
        lean, turn, roll = (a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f, a[4] + (b[4] - a[4]) * f)
        seat(sk, pose, lean=lean, rise=0.05, forward=0.04, turn=turn, roll=roll, look=0.6)
        reins(sk, pose, offset=(0.0, 0.0, 0.04 + 0.25 * np.sin(np.radians(lean))))
        hold_spear(sk, pose, mix(a[1][0], b[1][0]), mix(a[1][1], b[1][1]), pole=mix(a[1][2], b[1][2]))
        clip.key(pose)
    return clip.tracks(moved_joints(sk, base) | {"pelvis"})


def clips():
    sk = Skeleton()
    out = [
        bow_shoot(sk),
        spear_loop(sk, "Spear_Idle", "Sword_Idle", *READY),
        spear_loop(sk, "Spear_Rest", "Idle_Loop", (-0.25, 0.06, 0.12), (0.0, 1.0, 0.06), pole=(-1.0, -0.6, 0.6)),
        spear_loop(sk, "Spear_Walk", "Walk_Loop", (-0.25, 0.08, 0.15), (0.0, 1.0, 0.3), pole=(-1.0, -0.6, 0.6)),
        spear_loop(sk, "Spear_Jog", "Jog_Fwd_Loop", (-0.26, 0.14, 0.22), (0.0, 0.35, 1.0)),
        spear_thrust(sk, "Spear_Thrust"),
        spear_thrust(sk, "Spear_Thrust_Low", height=0.18, aim_down=0.16, lunge=0.2),
        ride_loop(sk, "Ride_Idle", lean=4.0, rise=0.0, forward=0.0, sword=RIDE_REST),
        ride_loop(sk, "Ride_Gallop", lean=20.0, rise=0.05, forward=0.04, sword=RIDE_CHARGE),
        ride_slash(sk),
    ]
    return out


if __name__ == "__main__":
    for c in clips():
        print(c["name"], len(c["tracks"]), "joints")
