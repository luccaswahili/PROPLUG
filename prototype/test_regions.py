import sys, importlib.util, numpy as np
from PIL import Image
def mod(p, n):
    s = importlib.util.spec_from_file_location(n, p); m = importlib.util.module_from_spec(s); s.loader.exec_module(m); return m
v1 = mod(__import__("os").path.join(__import__("os").path.dirname(__file__), "lookmatch_v1.py"), "v1"); v2 = mod(__import__("os").path.join(__import__("os").path.dirname(__file__), "lookmatch.py"), "v2")
SKIN = [(224,172,140),(198,134,66),(160,110,80),(110,70,50),(240,200,180)]
def scene(seed, mix, h=360, w=640):
    """mix: dict of material -> area share; returns image and skin mask"""
    r = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w] / np.array([h, w])[:, None, None]
    img = np.zeros((h, w, 3)); lab = np.zeros((h, w), int)
    mats = {"sky": lambda n: np.array([110,165,230])/255*(0.85+0.25*y[n][:,None]),
            "foliage": lambda n: np.array([70,125,50])/255*(0.6+0.6*r.random(n.sum())[:,None]),
            "wall": lambda n: np.array([150,150,145])/255*(0.5+0.6*x[n][:,None]),
            "skin": None}
    names = list(mix); cuts = np.cumsum([mix[k] for k in names])
    # vertical stripes with wobble so regions are contiguous
    pos = (x + 0.05*np.sin(y*12+seed)) 
    for i, k in enumerate(names):
        lo = 0 if i == 0 else cuts[i-1]; m = (pos >= lo) & (pos < cuts[i]) if i < len(names)-1 else pos >= lo
        if k == "skin":
            base = np.array(SKIN[0])/255
            img[m] = base * (0.55 + 0.6*(1-np.abs(x[m]-0.5))[:,None]) 
            lab[m] = 1
        else:
            img[m] = mats[k](m)
    img = np.clip(img + r.normal(0, 0.015, img.shape), 0, 1)
    return img, lab == 1
def grade(img):  # teal-orange, lifted blacks, desaturated, skin pushed slightly orange
    lab = v2.rgb_to_oklab(img.reshape(-1,3))
    lab[:,0] = 0.06 + 0.88*lab[:,0]**1.15
    lab[:,1:] *= 0.75
    t = np.clip((lab[:,0]-0.55)*2.5, -1, 1)
    lab[:,1] += np.where(t<0, -0.02, 0.012)*np.abs(t); lab[:,2] += np.where(t<0, -0.025, 0.03)*np.abs(t)
    return v2.oklab_to_rgb(lab).reshape(img.shape)
def hue(img, m):
    l = v2.rgb_to_oklab(img[m]); return np.degrees(np.arctan2(l[:,2].mean(), l[:,1].mean()))
def err(a, b, m=None):
    d = np.abs(a-b) if m is None else np.abs(a[m]-b[m]); return round(float(d.mean()*255), 2)
cases = {
  "sky-heavy reference on a face shot": ({"sky":.7,"foliage":.2,"skin":.1}, {"wall":.3,"skin":.6,"foliage":.1}),
  "similar content (sanity)":           ({"wall":.35,"skin":.5,"foliage":.15}, {"wall":.3,"skin":.6,"foliage":.1}),
  "foliage-heavy reference on a city wall": ({"foliage":.75,"wall":.15,"sky":.1}, {"wall":.7,"sky":.2,"skin":.1}),
}
rows = []
for i, (name, (rm, sm)) in enumerate(cases.items()):
    refi, _ = scene(10+i, rm); src, skin = scene(3+i, sm); ref = grade(refi); truth = grade(src)
    o1 = v1.LookModel(ref, src).apply_rgb(src)
    o2p = v2.LookModel(ref, src, skin_protect=0).apply_rgb(src)
    o2 = v2.LookModel(ref, src).apply_rgb(src)
    print(f"\n== {name}")
    print(f"  error vs ideal grade (8-bit mean abs): untouched {err(src,truth)} | v1 {err(o1,truth)} | v2 no-protect {err(o2p,truth)} | v2 default {err(o2,truth)}")
    if skin.any():
        print(f"  skin error: untouched {err(src,truth,skin)} | v1 {err(o1,truth,skin)} | v2 no-protect {err(o2p,truth,skin)} | v2 default {err(o2,truth,skin)}")
        print(f"  skin hue deg: orig {hue(src,skin):.1f} ideal {hue(truth,skin):.1f} v1 {hue(o1,skin):.1f} v2np {hue(o2p,skin):.1f} v2 {hue(o2,skin):.1f}")
    strip = np.concatenate([src, ref, o1, o2, truth], 1)
    Image.fromarray((np.clip(strip,0,1)*255+.5).astype(np.uint8)).resize((1600, 180)).save(f"case{i}.png")
