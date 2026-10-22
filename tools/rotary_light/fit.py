import ctypes, numpy as np, time, sys, json
from scipy.optimize import differential_evolution, minimize
lib=ctypes.CDLL('./liblight.so'); lib.cost.restype=ctypes.c_double; lib.cost.argtypes=[ctypes.POINTER(ctypes.c_double),ctypes.c_int]; lib.setmask.argtypes=[ctypes.c_int]
names=['FC','HP','HQ','HS1','HPH1','HA1','HB1','HGPH1','HS2','HPH2','HA2','HB2','HGPH2','PSI','DP','DQ','DA','DB','DGPH','DPSI','HL','DL','LK','HSLOPE']
x0=np.concatenate([np.load('merged.npy')[:24],np.load('tone.npy')[0]])
bounds=[(300,1500),(1500,12000),(-5500,-300),(0,1),(-0.5,0.5),(0,1),(0,1),(-0.5,0.5),(10,200),(-0.5,0.5),(0,1),(0,1),(-0.5,0.5),(0,1),
        (1500,12000),(-5500,-300),(0,1),(0,1),(-0.5,0.5),(0.3,0.7),(0,2),(0,2),(0,0.3),(0,1)]
for _i in (4,7,9,12,13,18,19): x0[_i]=(x0[_i]+0.5)%1.0-0.5 if _i!=13 and _i!=19 else x0[_i]%1.0
def cost(p,v=0):
    a=(ctypes.c_double*29)(*p); return lib.cost(a,v)
which=sys.argv[1]; iters=int(sys.argv[2])
idx={'drum':[14,15,16,17,18,19,21],'horn':[1,2,4,5,6,7,8,9,10,11,12,13,20]}[which]
lib.setmask.argtypes=[ctypes.c_int]; lib.setmask(2 if which=='drum' else 1)
def f(z):
    p=x0.copy(); p[idx]=z; return cost(p)
b=[bounds[i] for i in idx]
r=differential_evolution(f,b,x0=x0[idx],popsize=6,maxiter=iters,seed=3,updating='immediate',tol=1e-9,polish=False,init='sobol')
r2=minimize(f,r.x,method='Powell',options={'maxiter':4000,'xtol':1e-4,'ftol':1e-7})
p=x0.copy(); p[idx]=r2.x; np.save('best_%s.npy'%which,p)
print(which,'cost start',f(x0[idx]),'DE',r.fun,'Powell',r2.fun); print(dict(zip([names[i] for i in idx],np.round(r2.x,4))))
