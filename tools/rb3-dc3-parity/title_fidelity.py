"""Fidelity vs a reference frame (retail capture). Coarse, animation-tolerant.
Usage: title_fidelity.py REF img...   prints per-image metrics:
  sky_dE / city_dE : mean |dRGB| (0-255) between 16x16-box-blurred 320x180 downsamples, sky = top 30%, city = rest
  city_edge        : NCC of gradient magnitude (blurred 4px) over the city region (structure, 1=identical)
  lum              : mean luma (ref value printed on the REF line)
"""
import sys
import numpy as np
from PIL import Image, ImageFilter
W,H=320,180
def load(p):
    im=Image.open(p).convert('RGB').resize((W,H),Image.BILINEAR)
    return im
def arr(im): return np.asarray(im).astype(np.float32)
def stats(p):
    im=load(p)
    blur=arr(im.filter(ImageFilter.BoxBlur(8)))
    g=arr(im.convert('L').filter(ImageFilter.GaussianBlur(1.5)))
    gx=np.zeros_like(g); gy=np.zeros_like(g)
    gx[:,1:-1]=g[:,2:]-g[:,:-2]; gy[1:-1,:]=g[2:,:]-g[:-2,:]
    gm=np.hypot(gx,gy)
    gm=np.asarray(Image.fromarray(np.clip(gm,0,255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(4))).astype(np.float32)
    full=arr(im)
    luma=(full*[0.299,0.587,0.114]).sum(-1)
    return blur,gm,luma,full
ref=stats(sys.argv[1]); sk=int(H*0.30)
def ncc(a,b):
    a=a-a.mean(); b=b-b.mean(); return float((a*b).sum()/np.sqrt((a*a).sum()*(b*b).sum()+1e-9))
print(f"REF lum={ref[2].mean():.1f} sky_lum={ref[2][:sk].mean():.1f} city_lum={ref[2][sk:].mean():.1f}")
for p in sys.argv[2:]:
    s=stats(p)
    sky=np.abs(s[0][:sk]-ref[0][:sk]).mean(); city=np.abs(s[0][sk:]-ref[0][sk:]).mean()
    e=ncc(s[1][sk:],ref[1][sk:])
    sat=(s[3].max(-1)-s[3].min(-1)).mean()
    print(f"{'/'.join(p.split('/')[-2:]):40s} sky_dE={sky:5.1f} city_dE={city:5.1f} city_edge={e:.3f} lum={s[2].mean():5.1f} sky_lum={s[2][:sk].mean():5.1f} city_lum={s[2][sk:].mean():5.1f} chroma={sat:5.1f}")
