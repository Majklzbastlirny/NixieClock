"""Render each .3mf in this folder to an isometric shaded PNG (renders/<name>.png).
Throwaway helper; not part of the firmware build."""
import os, glob
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import trimesh

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "renders")
os.makedirs(OUT, exist_ok=True)

BASE = np.array([0.86, 0.45, 0.16])   # warm PETG-orange
LIGHT = np.array([0.4, -0.7, 0.6]); LIGHT = LIGHT / np.linalg.norm(LIGHT)

def load_mesh(path):
    m = trimesh.load(path, force="mesh")
    if isinstance(m, trimesh.Scene):
        m = trimesh.util.concatenate(tuple(m.geometry.values()))
    return m

def render(path):
    name = os.path.splitext(os.path.basename(path))[0]
    m = load_mesh(path)
    v, f = m.vertices, m.faces
    # center on origin
    v = v - (v.max(0) + v.min(0)) / 2.0
    tris = v[f]                                    # (n,3,3)

    # flat shading: face normal . light, clamped
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    ln = np.linalg.norm(n, axis=1); ln[ln == 0] = 1
    n = n / ln[:, None]
    shade = np.clip(np.abs(n @ LIGHT), 0.0, 1.0)
    inten = 0.35 + 0.65 * shade
    colors = np.clip(BASE[None, :] * inten[:, None], 0, 1)
    colors = np.hstack([colors, np.ones((len(colors), 1))])

    fig = plt.figure(figsize=(6, 6), dpi=200)
    ax = fig.add_subplot(111, projection="3d")
    pc = Poly3DCollection(tris, facecolors=colors, edgecolors=None, linewidths=0)
    ax.add_collection3d(pc)

    # True proportions: box aspect follows real dimensions so flat parts (panels,
    # feet) fill the frame instead of floating inside a forced cube.
    ext = v.max(0) - v.min(0)
    ext = np.where(ext < 1e-6, 1e-6, ext)
    pad = 0.04 * ext
    ax.set_xlim(-ext[0] / 2 - pad[0], ext[0] / 2 + pad[0])
    ax.set_ylim(-ext[1] / 2 - pad[1], ext[1] / 2 + pad[1])
    ax.set_zlim(-ext[2] / 2 - pad[2], ext[2] / 2 + pad[2])
    ax.set_box_aspect(tuple(ext))
    ax.view_init(elev=24, azim=-58)
    ax.set_axis_off()
    fig.patch.set_facecolor("white")
    ax.patch.set_alpha(0.0)
    try:
        ax.set_proj_type("persp", focal_length=0.6)
    except Exception:
        pass
    out = os.path.join(OUT, name + ".png")
    fig.savefig(out, facecolor="white", bbox_inches="tight", pad_inches=0.05)
    plt.close(fig)
    print(f"{name}: {len(f)} tris -> {out}")

for p in sorted(glob.glob(os.path.join(HERE, "*.3mf"))):
    render(p)
print("done")
