// ps2port trial: print R5900Context field offsets as JSON, so Python can
// build replay states (make_replay_state.py). Same on x86-64 and aarch64.
#include "ps2_runtime.h"

#include <cstddef>
#include <cstdio>

#define F(name) std::printf("\"%s\": %zu, ", #name, offsetof(R5900Context, name))

int main()
{
    std::printf("{");
    F(r); F(pc); F(hi); F(lo); F(hi1); F(lo1); F(sa);
    F(vu0_vf); F(vi); F(vu0_q); F(vu0_p); F(vu0_i); F(vu0_r); F(vu0_acc);
    F(vu0_status); F(vu0_mac_flags); F(vu0_clip_flags);
    F(cop0_status); F(f); F(fcr31);
    std::printf("\"size\": %zu}\n", sizeof(R5900Context));
    return 0;
}
