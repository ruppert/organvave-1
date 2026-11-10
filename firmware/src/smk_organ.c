/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The SMK-37 port's organ mapping (BOARD_SMK37 only; smk37-port.yaml): the 16 pads, the 4 faders, the wheels, the pedal,
 * the outer keys and the extra buttons, on top of the board layer of genkigenki/fm1-to-smk37 (hal/smk_input.h). main.c
 * calls smk_frame() once a UI frame (before the LEDs and the screen) and smk_poll() between frames. */
#ifndef SMK_ORGAN_C
#define SMK_ORGAN_C

static void smk_btn_leds(void)                 /* (organ_app.c oa_leds: KNOB BANK, FADER BANK) */
{
}

static void smk_frame(void)
{
}

static void smk_poll(void)
{
}

#endif /* SMK_ORGAN_C */
