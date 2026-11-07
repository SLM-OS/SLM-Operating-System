/*
 * slm_chunks.h - warp chunk arithmetic shared by the kernels and the host tests.
 * No CUDA dependency, so the host test checks the same expression the kernel uses.
 */
#ifndef SLM_CHUNKS_H
#define SLM_CHUNKS_H

/* 32-lane chunks needed to cover half_hd RoPE pairs, rounded up. */
#define SLM_ROPE_CHUNKS(half_hd) (((half_hd) + 31) >> 5)

#endif /* SLM_CHUNKS_H */
