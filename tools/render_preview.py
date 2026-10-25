#!/usr/bin/env python3
"""
AvatarForge :: tools/render_preview.py

Headless software preview of an exported .glb, rendered with the SAME camera the app uses by
default (yaw 180 / pitch 4 / distance 3.0 / targetY 0.95 / fov 32) so the image can be compared
1:1 against what the device viewer should show. No GPU, no GL: a z-buffer rasterizer over the
rest-pose (bind-pose) geometry with Lambert lighting and the material base-colour textures.

Usage:  python3 tools/render_preview.py artifacts/avatarforge-sample.glb preview.png [W H]
Needs:  numpy, Pillow   (pip install numpy pillow)
"""
import io
import struct
import sys

import numpy as np
from PIL import Image


def load_glb(path):
    data = open(path, "rb").read()
    magic, ver, length = struct.unpack("<III", data[:12])
    assert magic == 0x46546C67 and ver == 2, "not a glb2"
    clen, ctype = struct.unpack("<II", data[12:20])
    assert ctype == 0x4E4F534A
    js = json_load(data[20:20 + clen])
    off = 20 + clen
    off += (4 - off % 4) % 4
    blen, btype = struct.unpack("<II", data[off:off + 8])
    body = data[off + 8:off + 8 + blen]
    return js, body


def json_load(b):
    import json
    return json.loads(b)


def accessor(js, body, idx):
    a = js["accessors"][idx]
    bv = js["bufferViews"][a["bufferView"]]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    size = {"5120": 1, "5121": 1, "5122": 2, "5123": 2, "5125": 4, "5126": 4}[str(a["componentType"])]
    dtype = {"5120": np.uint8, "5121": np.int8, "5122": np.uint16, "5123": np.int16,
             "5125": np.uint32, "5126": np.float32}[str(a["componentType"])]
    base = bv.get("byteOffset", 0)
    stride = bv.get("byteStride", 0)
    n = a["count"]
    if stride:
        raw = np.frombuffer(body[base:base + n * stride], dtype)
        arr = raw.reshape(n, stride // size)[:, :ncomp].astype(np.float32)
    else:
        raw = np.frombuffer(body, dtype, offset=base, count=n * ncomp)
        arr = raw.reshape(n, ncomp).astype(np.float32)
    return arr[:n]


def look_at(eye, target, up):
    f = target - eye
    f /= np.linalg.norm(f)
    s = np.cross(f, up)
    s /= np.linalg.norm(s)
    u = np.cross(s, f)
    M = np.eye(4)
    M[0, :3], M[1, :3], M[2, :3] = s, u, -f
    M[0, 3], M[1, 3], M[2, 3] = -s @ eye, -u @ eye, f @ eye
    return M


def perspective(fov_deg, aspect, zn, zf):
    f = 1.0 / np.tan(np.deg2rad(fov_deg) / 2.0)
    M = np.zeros((4, 4))
    M[0, 0] = f / aspect
    M[1, 1] = f
    M[2, 2] = -(zf + zn) / (zf - zn)
    M[2, 3] = -2 * zf * zn / (zf - zn)
    M[3, 2] = -1.0
    return M


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "artifacts/avatarforge-sample.glb"
    out = sys.argv[2] if len(sys.argv) > 2 else "preview.png"
    W = int(sys.argv[3]) if len(sys.argv) > 3 else 900
    H = int(sys.argv[4]) if len(sys.argv) > 4 else 1920

    js, body = load_glb(path)

    # ---- textures (base colour, embedded sRGB) --------------------------------------
    tex_cache = {}

    def texture(img_idx):
        if img_idx in tex_cache:
            return tex_cache[img_idx]
        img = js["images"][img_idx]
        bv = js["bufferViews"][img["bufferView"]]
        raw = body[bv.get("byteOffset", 0):bv.get("byteOffset", 0) + bv["byteLength"]]
        pil = Image.open(io.BytesIO(raw)).convert("RGB")
        arr = np.asarray(pil).astype(np.float32) / 255.0
        arr = arr ** 2.2  # sRGB -> linear approx
        tex_cache[img_idx] = arr
        return arr

    def base_color(mat_idx, uv):
        mat = js["materials"][mat_idx]
        pbr = mat.get("pbrMetallicRoughness", {})
        factor = np.array(pbr.get("baseColorFactor", [1, 1, 1, 1])[:3], np.float32)
        texref = pbr.get("baseColorTexture")
        if texref is None:
            return np.tile(factor, (len(uv), 1))
        t = texture(js["textures"][texref["index"]]["source"])
        th, tw = t.shape[:2]
        # glTF UV origin is top-left; Pillow rows are top-down -> no flip
        x = np.clip((uv[:, 0] * tw).astype(np.int32), 0, tw - 1)
        y = np.clip((uv[:, 1] * th).astype(np.int32), 0, th - 1)
        return t[y, x] * factor

    # ---- gather geometry --------------------------------------------------------------
    pos_all, nrm_all, uv_all, mat_all, idx_all = [], [], [], [], []
    v_off = 0
    for prim in js["meshes"][0]["primitives"]:
        attrs = prim["attributes"]
        p = accessor(js, body, attrs["POSITION"])
        n = accessor(js, body, attrs["NORMAL"])
        uv = accessor(js, body, attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs \
            else np.zeros((len(p), 2), np.float32)
        i = accessor(js, body, prim["indices"])
        pos_all.append(p)
        nrm_all.append(n)
        uv_all.append(uv)
        mat_all.append(np.full(len(p), prim.get("material", 0), np.int32))
        idx_all.append(i.astype(np.int64) + v_off)
        v_off += len(p)
    P = np.concatenate(pos_all)
    N = np.concatenate(nrm_all)
    UV = np.concatenate(uv_all)
    MAT = np.concatenate(mat_all)
    I = np.concatenate(idx_all)
    tris = I.reshape(-1, 3)
    print(f"verts={len(P)} tris={len(tris)}")

    BC = np.empty((len(UV), 3), np.float32)
    for m in np.unique(MAT):
        sel = MAT == m
        BC[sel] = base_color(int(m), UV[sel])
    tex_cache.clear()  # textures are baked into BC now; free ~100s of MB

    # ---- camera: the app's default orbit ---------------------------------------------
    yaw, pitch, dist, target_y, fov = 180.0, 4.0, 3.0, 0.95, 32.0
    yaw_r, pitch_r = np.deg2rad(yaw), np.deg2rad(pitch)
    eye = np.array([dist * np.cos(pitch_r) * np.sin(yaw_r),
                    target_y + dist * np.sin(pitch_r),
                    dist * np.cos(pitch_r) * np.cos(yaw_r)])
    view = look_at(eye, np.array([0.0, target_y, 0.0]), np.array([0.0, 1.0, 0.0]))
    proj = perspective(fov, W / H, 0.05, 60.0)
    vp = proj @ view

    vp = vp.astype(np.float32)
    clip = (vp @ np.c_[P, np.ones(len(P), np.float32)].T).T
    w = np.where(clip[:, 3] <= 1e-5, 1e-5, clip[:, 3])
    ndc = clip[:, :3] / w[:, None]
    sx = (ndc[:, 0] * 0.5 + 0.5) * W
    sy = (1.0 - (ndc[:, 1] * 0.5 + 0.5)) * H
    sz = ndc[:, 2]

    tv = np.stack([sx[tris], sy[tris], sz[tris]], axis=2)  # (T, corner, x|y|z)
    X, Y = tv[:, :, 0], tv[:, :, 1]
    minx, maxx = X.min(axis=1), X.max(axis=1)
    miny, maxy = Y.min(axis=1), Y.max(axis=1)
    a2 = X[:, 0] * (Y[:, 1] - Y[:, 2]) + X[:, 1] * (Y[:, 2] - Y[:, 0]) + X[:, 2] * (Y[:, 0] - Y[:, 1])
    keep = (a2 > 1e-3) & (maxx >= 0) & (minx <= W) & (maxy >= 0) & (miny <= H)
    print(f"visible tris={keep.sum()}")

    tvi = tris[keep]
    t = tv[keep]
    x0 = np.clip(np.floor(minx[keep]).astype(np.int64), 0, W - 1)
    x1 = np.clip(np.ceil(maxx[keep]).astype(np.int64), 0, W - 1)
    y0 = np.clip(np.floor(miny[keep]).astype(np.int64), 0, H - 1)
    y1 = np.clip(np.ceil(maxy[keep]).astype(np.int64), 0, H - 1)
    den = a2[keep]
    A, B, C = t[:, 0], t[:, 1], t[:, 2]
    ABx, ABy = B[:, 0] - A[:, 0], B[:, 1] - A[:, 1]
    ACx, ACy = C[:, 0] - A[:, 0], C[:, 1] - A[:, 1]

    vc = BC[tvi]
    vn = N[tvi]
    # ---- lighting: the app's default sun (azimuth 42, elevation 38) -------------------
    az, el = np.deg2rad(42.0), np.deg2rad(38.0)
    L = np.array([np.cos(el) * np.sin(az), np.sin(el), np.cos(el) * np.cos(az)])
    L /= np.linalg.norm(L)
    sun_col = np.array([1.0, 0.96, 0.90], np.float32) * np.float32(1.35)
    amb_col = np.array([0.42, 0.47, 0.58], np.float32)
    ndl = np.clip(np.einsum("tij,j->ti", vn, L), 0, 1)
    back = np.clip(-np.einsum("tij,j->ti", vn, L), 0, 1) * 0.18
    shade = amb_col[None, None, :] + sun_col[None, None, :] * (ndl + back)[:, :, None]
    VCOL = np.clip(vc * shade, 0, 1) ** (1 / 2.2)

    # ---- z-buffer paint, chunked to bound memory --------------------------------------
    zbuf = np.full(H * W, 1e30, np.float32)
    img = np.zeros((H * W, 3), np.float32)
    areas = np.maximum(1, (x1 - x0 + 1) * (y1 - y0 + 1))
    order = np.argsort(areas)  # small bboxes first keeps chunks tight

    CH_TOTAL = 5_000_000
    n = len(order)
    i = 0
    while i < n:
        # grow the chunk until its candidate-pixel budget is spent
        j = i
        budget = CH_TOTAL
        while j < n and budget - int(areas[order[j]]) > 0:
            budget -= int(areas[order[j]])
            j += 1
        sel = order[i:j]                     # triangle ids in this chunk
        areas_c = areas[sel]
        pos = np.repeat(np.arange(len(sel)), areas_c)   # chunk position per candidate pixel
        start = np.concatenate([[0], np.cumsum(areas_c)[:-1]])
        idx = np.arange(int(areas_c.sum()), dtype=np.int64) - start[pos]
        tri_id = sel[pos]                    # triangle id per candidate pixel
        wcol = x1[tri_id] - x0[tri_id] + 1
        py = y0[tri_id] + idx // wcol
        px = x0[tri_id] + idx % wcol
        fp = py * W + px

        Ax = A[tri_id, 0].astype(np.float32); Ay = A[tri_id, 1].astype(np.float32)
        Az = A[tri_id, 2].astype(np.float32)
        Bx = B[tri_id, 0].astype(np.float32); By = B[tri_id, 1].astype(np.float32)
        Bz = B[tri_id, 2].astype(np.float32)
        Cx = C[tri_id, 0].astype(np.float32); Cy = C[tri_id, 1].astype(np.float32)
        Cz = C[tri_id, 2].astype(np.float32)
        px_f = px.astype(np.float32)
        py_f = py.astype(np.float32)
        v0x = ABx[tri_id].astype(np.float32); v0y = ABy[tri_id].astype(np.float32)
        v1x = ACx[tri_id].astype(np.float32); v1y = ACy[tri_id].astype(np.float32)
        d00 = v0x * v0x + v0y * v0y
        d01 = v0x * v1x + v0y * v1y
        d11 = v1x * v1x + v1y * v1y
        vx, vy = px_f - Ax, py_f - Ay
        d20 = vx * v0x + vy * v0y
        d21 = vx * v1x + vy * v1y
        D = d00 * d11 - d01 * d01
        u = (d11 * d20 - d01 * d21) / D
        v = (d00 * d21 - d01 * d20) / D
        inside = (u >= -1e-4) & (v >= -1e-4) & (u + v <= 1.0 + 1e-4)
        m = np.where(inside)[0]
        if len(m):
            uu, vv = u[m], v[m]
            z = Az[tri_id[m]] + uu * (Bz[tri_id[m]] - Az[tri_id[m]]) + vv * (Cz[tri_id[m]] - Az[tri_id[m]])
            cA = VCOL[tri_id[m], 0]
            cB = VCOL[tri_id[m], 1]
            cC = VCOL[tri_id[m], 2]
            col = (cA + uu[:, None] * (cB - cA) + vv[:, None] * (cC - cA)).astype(np.float32)
            z = z.astype(np.float32)
            fp_m = fp[m]
            ord_ = np.argsort(z, kind="stable")
            z_s, fp_s, col_s = z[ord_], fp_m[ord_], col[ord_]
            take = z_s < zbuf[fp_s]
            zbuf[fp_s[take]] = z_s[take]
            img[fp_s[take]] = col_s[take]
        i = j
        print(f"  painted {j}/{n} tris", flush=True)

    # ---- background: the app's clear colour with a slight vertical lift ---------------
    rows = (np.arange(H, dtype=np.float32) / H)
    bg = np.zeros((H, W, 3), np.float32)
    top = np.array([0.075, 0.095, 0.14], np.float32)
    bot = np.array([0.055, 0.075, 0.11], np.float32)
    grad = bot[None, :] + (top - bot)[None, :] * (1.0 - rows)[:, None]
    bg[:] = grad[:, None, :]
    painted = zbuf < 1e29
    out_img = np.where(painted[:, None], img, bg.reshape(H * W, 3)).reshape(H, W, 3)
    Image.fromarray((np.clip(out_img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(out)
    print(f"wrote {out} ({W}x{H})")


if __name__ == "__main__":
    main()
