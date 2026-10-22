import numpy as np, subprocess, os, sys
RR=os.environ.get('SBF_REF','build/sbf_ref')   # the directory tools/setbfree_ref/build.sh builds (whirl_render is built with CONST support below)
FS=44100
# cases: (kind, tone, rpm_h, rpm_d) ; kind 'h': horn only (drum stopped), 'd': drum only (horn stopped)
cases=[]
for rpm in (400,48):
    for f in (700,1000,2000,3500,5000): cases.append(('h',f,rpm,0))
for rpm in (340,40):
    for f in (100,200,300,500): cases.append(('d',f,0,rpm))
def sb(y,f,fr,n0,W):
    n=np.arange(W); w=0.5-0.5*np.cos(2*np.pi*n/W); y=y[n0:n0+W]
    out=[]
    for k in range(-4,5):
        a=2*abs(np.sum(w*y*np.exp(-2j*np.pi*(f+k*fr)*n/FS)*np.exp(-2j*np.pi*(f+k*fr)*n0/FS)))/w.sum()
        out.append(20*np.log10(max(a/0.5,1e-3)))   # dB re the input amplitude, floor -60
    return out
lines=[]
for kind,f,rh,rd in cases:
    fr=(rh if kind=='h' else rd)/60.0
    T=FS/fr; per=6 if (rh>100 or rd>100) else 2
    n0=int(round(2*T)); W=int(round(per*T)); N=n0+W
    t=np.arange(N)/FS; x=(0.5*np.sin(2*np.pi*f*t)).astype(np.float32); x.tofile('tmp_in.f32')
    env=dict(os.environ,CONST='1')
    subprocess.run([RR+'/whirl_render','tmp_in.f32','tmp_out.f32',str(rh),str(rd)],check=True,env=env)
    y=np.fromfile('tmp_out.f32',dtype=np.float32).reshape(-1,2).astype(float)
    row=[0 if kind=='h' else 1,f,fr,n0,W]+sb(y[:,0],f,fr,n0,W)+sb(y[:,1],f,fr,n0,W)
    lines.append(' '.join('%.6g'%v for v in row))
    print(kind,f,rh,rd,'L',np.round(row[5:14],1),'R',np.round(row[14:],1))
open('ref.txt','w').write('\n'.join(lines)+'\n')
