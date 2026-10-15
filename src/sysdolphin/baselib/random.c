#include "random.h"

static u32 seed = 1;
u32* HSD_RandSeedPtr = &seed;

#ifdef TARGET_PC
/* MELEE_NET_DET_TRACE (src/pc/net_snapshot.c): every draw, with its caller,
 * while a determinism trace window is open. NULL otherwise. */
void (*pc_rand_trace)(void* caller, u32 seed_after);
#define RAND_TRACE()                                                         \
    do {                                                                     \
        if (pc_rand_trace != NULL) {                                         \
            pc_rand_trace(__builtin_return_address(0), *HSD_RandSeedPtr);    \
        }                                                                    \
    } while (0)
#else
#define RAND_TRACE() ((void) 0)
#endif

s32 HSD_Rand(void)
{
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    RAND_TRACE();
    return *HSD_RandSeedPtr >> 0x10;
}

f32 HSD_Randf(void)
{
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    RAND_TRACE();
    return (f32) (*HSD_RandSeedPtr >> 0x10) / (1 << 16);
}

s32 HSD_Randi(s32 max_val)
{
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    RAND_TRACE();
    return max_val * (s32) (*HSD_RandSeedPtr >> 0x10) / (1 << 16);
}

void _HSD_RandForgetMemory(void* low, void* high)
{
    if (low <= (void*) HSD_RandSeedPtr && (void*) HSD_RandSeedPtr < high) {
        HSD_RandSeedPtr = &seed;
    }
}
