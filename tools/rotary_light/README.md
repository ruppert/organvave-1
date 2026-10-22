# Rotary LIGHT: the fit

The LIGHT model (`firmware/src/eng_rotary_light.c`) has no reference of its own: it is fitted to CLASSIC's sidebands
(CLASSIC is checked against setBfree, `tests/rotary_test.c`). Pipeline:

1. `python3 mkref.py` (needs `SBF_REF`, the directory `tools/setbfree_ref/build.sh` builds) renders CLASSIC (setBfree's
   `whirl_render`, constant speeds) for nine tones and writes `ref.txt`: the nine sidebands (k = -4 .. 4 times the
   rotor's rate) of each, left and right.
2. `light_fit.c` (`cc -O3 -ffast-math -shared -fPIC -o liblight.so light_fit.c -lm`) is the float model: a crossover
   (a low-pass biquad), the horn with a direct and a reflected tap per mic (delay sqrt(P + Q cos), gain a + b cos), the
   drum with one tap per mic, and a tone biquad on the horn. `fit.py horn N` / `fit.py drum N` run a differential
   evolution and Powell over the horn's / the drum's parameters against `ref.txt`.
3. `fit.json` is the result (plus the tone biquads, fitted to CLASSIC's horn filter chain above 500 Hz: 1.0 dB rms
   for the default set, 4 dB for the sharp one, `ROT_CPL` sets 0, 1 and 3, 4). The drum's level was raised by 2.5 dB by hand.
4. `python3 gen_light_params.py` writes `firmware/src/eng_rotary_light_params.h` from it.

Result (the float model): the sidebands within 5 to 8 dB rms of CLASSIC's, the same at the slow speeds that were not in
the fit. The integer engine is within 0.5 dB of the float model. It is a character match (the rates, the depth of the
modulation, the left / right difference), not a sideband-by-sideband one: CLASSIC's deep combs come from a dozen taps
and an angle-dependent FIR that two taps cannot reproduce.
