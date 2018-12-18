    .text
    .arch   armv8-a

/* int utf8_range_asm(const unsigned char *data, int len) */
    .global utf8_range_asm

/* Arguments */
x_data              .req    x0
w_len               .req    w1
x_len               .req    x1

/* Returns */
w_valid             .req    w0

/* Local variables */
x_data_end          .req    x2
w_error             .req    w2
w_token4            .req    w3
w_tmp               .req    w4
/* neon tables */
v_first_len_tbl     .req    v0
q_first_len_tbl     .req    q0
v_first_range_tbl   .req    v1
q_first_range_tbl   .req    q1
v_range_min_tbl     .req    v2
q_range_min_tbl     .req    q2
v_range_max_tbl     .req    v3
q_range_max_tbl     .req    q3
v_range_adj_tbl1    .req    v4
q_range_adj_tbl1    .req    q4
v_range_adj_tbl2    .req    v5
q_range_adj_tbl2    .req    q5
/* neon constants */
v_const_1           .req    v6
v_const_2           .req    v7
v_const_e0          .req    v16
x_tbl_base          .req    x2
/* neon persistent */
v_error             .req    v17
b_error             .req    b17
v_input             .req    v18
q_input             .req    q18
v_prev_input        .req    v19
v_first_len         .req    v20
v_prev_first_len    .req    v21
v_range             .req    v22
/* neon temporary */
v_high_nibbles      .req    v25
v_tmp1              .req    v25
v_tmp2              .req    v26
v_shift1            .req    v25
v_pos               .req    v26
v_min               .req    v27
v_max               .req    v28

utf8_range_asm:
    cmp     w_len, #16
    blt     utf8_naive

    /* constants */
    movi    v_const_1.16b, #1
    movi    v_const_2.16b, #2
    movi    v_const_e0.16b, #0xE0

    /* tables */
    adrp    x_tbl_base, .Ltbl_base
    add     x_tbl_base, x_tbl_base, :lo12:.Ltbl_base
    ldp     q_first_len_tbl, q_first_range_tbl, [x_tbl_base]
    ldp     q_range_min_tbl, q_range_max_tbl, [x_tbl_base, #32]
    ldp     q_range_adj_tbl1, q_range_adj_tbl2, [x_tbl_base, #64]

    /* initialize */
    movi    v_error.4s, #0
    orr     v_prev_input.16b, v_error.16b, v_error.16b
    orr     v_prev_first_len.16b, v_error.16b, v_error.16b

    /* x_data_end = x_data + x_len - 16 */
    add     x_data_end, x_data, x_len
    sub     x_data_end, x_data_end, #16

.Lprocess16:
    ldr     q_input, [x_data], #16

    /* high_nibbles = input >> 4 */
    ushr    v_high_nibbles.16b, v_input.16b, #4

    /* first_len = first_len_tbl[high_nibbles] */
    tbl     v_first_len.16b, {v_first_len_tbl.16b}, v_high_nibbles.16b

    /* range = first_range_tbl[high_nibbles] */
    tbl     v_range.16b, {v_first_range_tbl.16b}, v_high_nibbles.16b

    /* range |= (first_len, prev_first_len) << 1 byte */
    ext     v_tmp2.16b, v_prev_first_len.16b, v_first_len.16b, #15
    orr     v_range.16b, v_range.16b, v_tmp2.16b

    /* saturate_sub(first_len, 1) */
    uqsub   v_tmp1.16b, v_first_len.16b, v_const_1.16b
    uqsub   v_tmp2.16b, v_prev_first_len.16b, v_const_1.16b
    /* range |= (tmp1, tmp2) << 2 bytes */
    ext     v_tmp1.16b, v_tmp2.16b, v_tmp1.16b, #14
    orr     v_range.16b, v_range.16b, v_tmp1.16b

    /* saturate_sub(first_len, 2) */
    uqsub   v_tmp1.16b, v_first_len.16b, v_const_2.16b
    uqsub   v_tmp2.16b, v_prev_first_len.16b, v_const_2.16b
    /* range |= (tmp1, tmp2) << 3 bytes */
    ext     v_tmp1.16b, v_tmp2.16b, v_tmp1.16b, #13
    orr     v_range.16b, v_range.16b, v_tmp1.16b

    /* shift1 = (input, prev_input) << 1 */
    ext     v_shift1.16b, v_prev_input.16b, v_input.16b, #15
    /* pos = shift1 - 0xE0 */
    sub     v_pos.16b, v_shift1.16b, v_const_e0.16b
    /* range += range_adjust_tbl[pos] */
    tbl     v_tmp1.16b, {v_range_adj_tbl1.16b - v_range_adj_tbl2.16b}, v_pos.16b
    add     v_range.16b, v_range.16b, v_tmp1.16b

    /* min = range_min_tbl[range], max = range_max_tbl[range] */
    tbl     v_min.16b, {v_range_min_tbl.16b}, v_range.16b
    tbl     v_max.16b, {v_range_max_tbl.16b}, v_range.16b

    /* error |= (input < min ||  input > max) */
    cmhi    v_tmp1.16b, v_min.16b, v_input.16b
    cmhi    v_tmp2.16b, v_input.16b, v_max.16b
    orr     v_error.16b, v_error.16b, v_tmp1.16b
    orr     v_error.16b, v_error.16b, v_tmp2.16b

    mov     v_prev_input.16b, v_input.16b
    mov     v_prev_first_len.16b, v_first_len.16b

    cmp     x_data, x_data_end
    bls     .Lprocess16

.Lprocess16_ends:

    /* len = len % 16 */
    and     w_len, w_len, #0xF

    /* reduce and check error */
    umaxv   b_error, v_error.16b
    umov    w_error, v_error.b[0]
    cbnz    w_error, .Lerror

    /* find last non 80~BF */
    umov    w_token4, v_prev_input.s[3]
    /* token4[3] not within 80~BF */
    sbfx    w_tmp, w_token4, #24, #8
    cmp     w_tmp, #-64 /* 0xC0 */
    bge     .Lnaive1
    /* token4[2] not with 80~BF */
    sbfx    w_tmp, w_token4, #16, #8
    cmp     w_tmp, #-64
    bge     .Lnaive2
    /* token4[1] not with 80~BF */
    sbfx    w_tmp, w_token4, #8, #8
    cmp     w_tmp, #-64
    bge     .Lnaive3

    /* we're at character boundary */
    b       utf8_naive

.Lnaive1:
    sub     x_data, x_data, #1
    add     w_len, w_len, #1
    b       utf8_naive

.Lnaive2:
    sub     x_data, x_data, #2
    add     w_len, w_len, #2
    b       utf8_naive

.Lnaive3:
    sub     x_data, x_data, #3
    add     w_len, w_len, #3
    b       utf8_naive

.Lerror:
    mov     w_valid, #0
    ret

    .section .rodata
    .balign 16

.Ltbl_base:
    /* first_len_tbl */
    .byte   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3
    /* first_range_tbl */
    .byte   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8
    /* range_min_tbl */
    .byte   0x00, 0x80, 0x80, 0x80, 0xA0, 0x80, 0x90, 0x80
    .byte   0xC2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    /* range_max_tbl */
    .byte   0x7F, 0xBF, 0xBF, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F
    .byte   0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    /* range_adjust_tbl */
    .byte   2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0
    .byte   3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

#if 0
    /* debug: output v0 */
    stp     x29, x30, [sp, #-16]!
    stp     x0, xzr, [sp, #-16]!
    mov     x0, #0
    mov     v0.16b, v_input.16b
    bl      print128
    mov     v0.16b, v_range.16b
    bl      print128
    ldp     x0, xzr, [sp], #16
    ldp     x29, x30, [sp], #16
#endif
