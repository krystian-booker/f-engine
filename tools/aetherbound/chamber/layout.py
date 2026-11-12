"""Where everything stands: shared by the floor painting, the geometry and map.json.
Engine coordinates: +x east, +z south; angles counter-clockwise from east, 90 = north."""
import math

SIZE = 32
CX, CZ = 16.0, 16.0          # centre of the round hall
RADIUS = 13.0                # inner face of the ring wall
WALL = 1.6                   # its thickness
DX, DZ = 11.5, 16.0          # the dais / the crater
DAIS = [(3.5, 0.25), (3.1, 0.5), (2.7, 0.75)]   # (radius, top height) of each step
DAIS_TOP = 0.75
RING = 4.1                   # the conduit ring round the dais
PILLAR_R = 11.4              # ring of pillars
PILLAR_ANGLES = [18.0, 46.8, 75.6, 104.4, 133.2, 162.0, 196.0]   # standing ones; the south is open to the camera
STUMP_ANGLES = [228.0, 262.0, 296.0, 318.0]                        # broken, under 1.3 units
PASSAGE = (27, 14, 31, 18)   # x0, z0, x1, z1 of the east passage floor


def polar(r, a, cx=CX, cz=CZ):
    return cx + r * math.cos(math.radians(a)), cz - r * math.sin(math.radians(a))


def ring_point(a, r=RING):
    return polar(r, a, DX, DZ)


# Conduits inlaid in the floor: polylines of (x, z) that meet the ring round the dais.
CONDUITS = [
    [ring_point(0), (25.2, 16.0)],                                     # the axis, to the door
    [(17.0, 5.6), (17.0, 10.5), ring_point(45)],                       # from the engine
    [(11.5, 5.9), ring_point(90)],                                     # from the great tank
    [(6.2, 10.0), (6.2, 10.7), ring_point(135)],                       # from the west pump
    [ring_point(180), (3.6, 16.0)],
    [ring_point(225), (5.6, 21.9), (5.6, 24.2)],
    [ring_point(270), (11.5, 27.6)],
    [ring_point(315), (17.4, 21.9), (17.4, 27.2)],
]
NODES = [((25.2, 16.0), 0.62), ((5.6, 24.2), 0.45), ((17.4, 27.2), 0.45), ((11.5, 27.6), 0.45), ((3.6, 16.0), 0.45)]

BRAZIERS = [  # x, z, lit in the chamber
    (18.6, 10.6), (24.6, 11.6), (6.0, 13.2), (13.2, 8.6),   # the back row, before the machinery
    (26.5, 19.5),                                           # by the door
    (7.6, 21.6), (24.2, 22.4),                              # the front row: low, soft foreground
]

POINTS = {  # name: (x, z, facing)
    "start": (28.5, 16.0, "w"), "boss": (11.5, 16.0, "e"), "centre": (16.0, 16.0, "w"),
    "intro1": (23.0, 14.5, "w"), "intro2": (24.2, 15.5, "w"), "intro3": (23.4, 16.6, "w"), "intro4": (24.5, 17.5, "w"),
    "party1": (20.5, 13.6, "w"), "party2": (21.3, 15.2, "w"), "party3": (20.7, 16.8, "w"), "party4": (21.5, 18.4, "w"),
    "thrown1": (24.5, 13.6, "w"), "thrown2": (25.3, 15.2, "w"), "thrown3": (24.7, 16.8, "w"),
    "thrown4": (25.5, 18.4, "w"),
    "rowan_walk": (15.5, 16.0, "w"), "cael_stop": (18.5, 16.0, "w"),
}
RUINS_POINTS = {
    "cael_wake": (22.5, 17.5, "w"), "darius_wake": (24.5, 16.0, "w"), "lyra_kneel": (13.2, 15.4, "sw"),
    "sword": (12.5, 16.0, "w"), "cael_run_end": (14.5, 16.6, "w"),
}
FACING = {"w": (-1.0, 0.0), "e": (1.0, 0.0), "n": (0.0, -1.0), "s": (0.0, 1.0), "sw": (-0.7071, 0.7071)}

# The crater of the ruins: a shallow scorched bowl, walkable. Depth by distance from its centre.
CRATER_R = 3.7


def crater_depth(d):
    """Depth below the floor of the bowl surface at distance d from the crater's centre."""
    if d >= CRATER_R:
        return 0.0
    k = 1.0 - d / CRATER_R
    return 0.62 * (k * k * (3 - 2 * k)) ** 0.9
