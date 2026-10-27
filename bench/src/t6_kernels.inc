/* T6 kernels in the shape a static recompiler emits: one C statement per
   R5900 instruction (the guest code is in the comments), 32-bit results
   sign-extended into 64-bit registers, branch conditions evaluated before
   the delay slot, guest memory through gptr(). Included three times by
   t6_ee.c with different REG / FR / QR / COUNT definitions:
     _ctx  registers live in the ee_ctx struct (naive recompiler output)
     _loc  registers are C locals, synced at entry/exit (register-allocating)
     _cnt  like _ctx, plus COUNT(n) per basic block (exact guest counts)   */

#define KCAT2(a, b) a##b
#define KCAT(a, b) KCAT2(a, b)

/* Object update loop: 64-byte objects {state, x, y, vx, vy, timer, flags, score}. */
static void KCAT(k_int, KSUFFIX)(ee_ctx *c, uint32_t base, uint32_t nobj)
{
    KDECL
    REG(4) = SE32(base);              /* a0 = first object */
    REG(5) = SE32(base + nobj * 64u); /* a1 = end */
    REG(16) = SE32(0x10000);          /* s0 = bound */
    for (;;) {
        COUNT(5);
        REG(8) = SE32(LW(c, (uint32_t)REG(4) + 0));   /* lw   t0, 0(a0)   */
        REG(9) = SE32(LW(c, (uint32_t)REG(4) + 4));   /* lw   t1, 4(a0)   */
        REG(10) = SE32(LW(c, (uint32_t)REG(4) + 12)); /* lw   t2, 12(a0)  */
        {
            int br = REG(8) == 0;                      /* beq  t0, zero, skip */
            REG(11) = SE32(LW(c, (uint32_t)REG(4) + 8)); /* lw t3, 8(a0)  (delay) */
            if (br)
                goto skip;
        }
        COUNT(6);
        REG(9) = SE32((uint32_t)REG(9) + (uint32_t)REG(10));  /* addu t1, t1, t2 */
        REG(12) = SE32(LW(c, (uint32_t)REG(4) + 16));        /* lw   t4, 16(a0) */
        REG(11) = SE32((uint32_t)REG(11) + (uint32_t)REG(12)); /* addu t3, t3, t4 */
        REG(13) = REG(9) < REG(16);                           /* sltu t5, t1, s0 */
        {
            int br = REG(13) != 0;                            /* bne  t5, zero, xok */
            SW(c, (uint32_t)REG(4) + 4, (uint32_t)REG(9));    /* sw   t1, 4(a0) (delay) */
            if (br)
                goto xok;
        }
        COUNT(2);
        REG(10) = SE32(0u - (uint32_t)REG(10));               /* subu t2, zero, t2 */
        SW(c, (uint32_t)REG(4) + 12, (uint32_t)REG(10));      /* sw   t2, 12(a0) */
    xok:
        COUNT(3);
        REG(13) = REG(11) < REG(16);                          /* sltu t5, t3, s0 */
        {
            int br = REG(13) != 0;                            /* bne  t5, zero, yok */
            SW(c, (uint32_t)REG(4) + 8, (uint32_t)REG(11));   /* sw   t3, 8(a0) (delay) */
            if (br)
                goto yok;
        }
        COUNT(2);
        REG(12) = SE32(0u - (uint32_t)REG(12));               /* subu t4, zero, t4 */
        SW(c, (uint32_t)REG(4) + 16, (uint32_t)REG(12));      /* sw   t4, 16(a0) */
    yok:
        COUNT(4);
        REG(14) = SE32(LW(c, (uint32_t)REG(4) + 20));         /* lw    t6, 20(a0) */
        REG(14) = SE32((uint32_t)REG(14) - 1u);               /* addiu t6, t6, -1 */
        {
            int br = (int64_t)REG(14) > 0;                    /* bgtz  t6, tok */
            SW(c, (uint32_t)REG(4) + 20, (uint32_t)REG(14));  /* sw    t6, 20(a0) (delay) */
            if (br)
                goto tok;
        }
        COUNT(5);
        REG(15) = SE32(LW(c, (uint32_t)REG(4) + 24));         /* lw    t7, 24(a0) */
        REG(15) = REG(15) ^ 1u;                               /* xori  t7, t7, 1 */
        SW(c, (uint32_t)REG(4) + 24, (uint32_t)REG(15));      /* sw    t7, 24(a0) */
        REG(14) = SE32(60);                                   /* addiu t6, zero, 60 */
        SW(c, (uint32_t)REG(4) + 20, (uint32_t)REG(14));      /* sw    t6, 20(a0) */
    tok:
        COUNT(5);
        REG(24) = SE32(LW(c, (uint32_t)REG(4) + 28));         /* lw   t8, 28(a0) */
        REG(25) = SE32((uint32_t)REG(9) << 2);                /* sll  t9, t1, 2 */
        REG(24) = REG(24) ^ REG(25);                          /* xor  t8, t8, t9 */
        REG(24) = SE32((uint32_t)REG(24) + (uint32_t)REG(11)); /* addu t8, t8, t3 */
        SW(c, (uint32_t)REG(4) + 28, (uint32_t)REG(24));      /* sw   t8, 28(a0) */
    skip:
        COUNT(3);
        REG(4) = SE32((uint32_t)REG(4) + 64u);                /* addiu a0, a0, 64 */
        if (REG(4) == REG(5))                                 /* bne  a0, a1, loop */
            break;                                            /* nop  (delay) */
    }
    KSYNC
}

/* Scalar FPU: p' = M p + v dt, clamp |p'| to R. 64-byte objects
   {px, py, pz, vx, vy, vz, ...}; constants {M[9], dt, R^2, R} at kconst. */
static void KCAT(k_fpu, KSUFFIX)(ee_ctx *c, uint32_t base, uint32_t nobj, uint32_t kconst)
{
    KDECL
    COUNT(12);                                               /* lwc1 f20..f31, 0..44(kconst) */
    FR(20) = LWC1(c, kconst + 0);
    FR(21) = LWC1(c, kconst + 4);
    FR(22) = LWC1(c, kconst + 8);
    FR(23) = LWC1(c, kconst + 12);
    FR(24) = LWC1(c, kconst + 16);
    FR(25) = LWC1(c, kconst + 20);
    FR(26) = LWC1(c, kconst + 24);
    FR(27) = LWC1(c, kconst + 28);
    FR(28) = LWC1(c, kconst + 32);
    FR(29) = LWC1(c, kconst + 36);
    FR(30) = LWC1(c, kconst + 40);
    FR(31) = LWC1(c, kconst + 44);
    REG(4) = SE32(base);
    REG(5) = SE32(base + nobj * 64u);
    for (;;) {
        COUNT(35);
        FR(0) = LWC1(c, (uint32_t)REG(4) + 0);               /* lwc1 f0, 0(a0) */
        FR(1) = LWC1(c, (uint32_t)REG(4) + 4);               /* lwc1 f1, 4(a0) */
        FR(2) = LWC1(c, (uint32_t)REG(4) + 8);               /* lwc1 f2, 8(a0) */
        FR(3) = FR(20) * FR(0);                              /* mul.s f3, f20, f0 */
        FR(4) = FR(21) * FR(1);                              /* mul.s f4, f21, f1 */
        FR(3) = FR(3) + FR(4);                               /* add.s f3, f3, f4 */
        FR(4) = FR(22) * FR(2);                              /* mul.s f4, f22, f2 */
        FR(3) = FR(3) + FR(4);                               /* add.s f3, f3, f4 */
        FR(5) = FR(23) * FR(0);
        FR(6) = FR(24) * FR(1);
        FR(5) = FR(5) + FR(6);
        FR(6) = FR(25) * FR(2);
        FR(5) = FR(5) + FR(6);
        FR(7) = FR(26) * FR(0);
        FR(8) = FR(27) * FR(1);
        FR(7) = FR(7) + FR(8);
        FR(8) = FR(28) * FR(2);
        FR(7) = FR(7) + FR(8);
        FR(9) = LWC1(c, (uint32_t)REG(4) + 12);              /* lwc1 f9, 12(a0) */
        FR(10) = LWC1(c, (uint32_t)REG(4) + 16);
        FR(11) = LWC1(c, (uint32_t)REG(4) + 20);
        FR(9) = FR(9) * FR(29);                              /* mul.s f9, f9, f29 */
        FR(3) = FR(3) + FR(9);                               /* add.s f3, f3, f9 */
        FR(10) = FR(10) * FR(29);
        FR(5) = FR(5) + FR(10);
        FR(11) = FR(11) * FR(29);
        FR(7) = FR(7) + FR(11);
        FR(12) = FR(3) * FR(3);                              /* mul.s f12, f3, f3 */
        FR(13) = FR(5) * FR(5);
        FR(12) = FR(12) + FR(13);
        FR(13) = FR(7) * FR(7);
        FR(12) = FR(12) + FR(13);
        c->fcr31 = FR(30) < FR(12) ? c->fcr31 | (1u << 23) : c->fcr31 & ~(1u << 23); /* c.lt.s f30, f12 */
        if (!(c->fcr31 & (1u << 23)))                        /* bc1f store; nop */
            goto store;
        COUNT(5);
        FR(13) = sqrtf(FR(12));                              /* sqrt.s f13, f12 */
        FR(13) = FR(31) / FR(13);                            /* div.s  f13, f31, f13 */
        FR(3) = FR(3) * FR(13);
        FR(5) = FR(5) * FR(13);
        FR(7) = FR(7) * FR(13);
    store:
        COUNT(6);
        SWC1(c, (uint32_t)REG(4) + 0, FR(3));                /* swc1 f3, 0(a0) */
        SWC1(c, (uint32_t)REG(4) + 4, FR(5));
        SWC1(c, (uint32_t)REG(4) + 8, FR(7));
        REG(4) = SE32((uint32_t)REG(4) + 64u);               /* addiu a0, a0, 64 */
        if (REG(4) == REG(5))                                /* bne a0, a1, loop; nop */
            break;
    }
    KSYNC
}

/* 128-bit moves, as GCC emits for struct copies and register save/restore. */
static void KCAT(k_lqsq, KSUFFIX)(ee_ctx *c, uint32_t src, uint32_t dst, uint32_t nq32)
{
    KDECL
    REG(4) = SE32(src);
    REG(5) = SE32(dst);
    REG(6) = SE32(nq32);
    for (;;) {
        COUNT(9);
        LQ(c, &QR(8), (uint32_t)REG(4) + 0);                 /* lq    t0, 0(a0) */
        LQ(c, &QR(9), (uint32_t)REG(4) + 16);                /* lq    t1, 16(a0) */
        SQ(c, &QR(8), (uint32_t)REG(5) + 0);                 /* sq    t0, 0(a1) */
        SQ(c, &QR(9), (uint32_t)REG(5) + 16);                /* sq    t1, 16(a1) */
        REG(4) = SE32((uint32_t)REG(4) + 32u);               /* addiu a0, a0, 32 */
        REG(5) = SE32((uint32_t)REG(5) + 32u);               /* addiu a1, a1, 32 */
        REG(6) = SE32((uint32_t)REG(6) - 1u);                /* addiu a2, a2, -1 */
        if (REG(6) == 0)                                     /* bne a2, zero, loop; nop */
            break;
    }
    KSYNC
}

/* Dependent loads/stores through the address-translation path; the next
   index depends on the loaded value (latency-bound, like pointer chasing). */
static void KCAT(k_mem, KSUFFIX)(ee_ctx *c, uint32_t base, uint32_t mask, uint32_t iters)
{
    KDECL
    REG(17) = SE32(mask);  /* s1 */
    REG(18) = SE32(base);  /* s2 */
    REG(6) = SE32(iters);
    for (;;) {
        COUNT(11);
        REG(9) = SE32((uint32_t)REG(8) << 2);                 /* sll   t1, t0, 2 */
        REG(9) = REG(9) & REG(17);                            /* and   t1, t1, s1 */
        REG(9) = SE32((uint32_t)REG(9) + (uint32_t)REG(18));  /* addu  t1, t1, s2 */
        REG(10) = SE32(LW(c, (uint32_t)REG(9)));              /* lw    t2, 0(t1) */
        REG(11) = SE32((uint32_t)REG(11) + (uint32_t)REG(10)); /* addu t3, t3, t2 */
        REG(8) = REG(8) ^ REG(10);                            /* xor   t0, t0, t2 */
        REG(8) = SE32((uint32_t)REG(8) + 0x3Du);              /* addiu t0, t0, 0x3d */
        SW(c, (uint32_t)REG(9) + 4, (uint32_t)REG(11));       /* sw    t3, 4(t1) */
        REG(6) = SE32((uint32_t)REG(6) - 1u);                 /* addiu a2, a2, -1 */
        if (REG(6) == 0)                                      /* bne a2, zero, loop; nop */
            break;
    }
    KSYNC
}
