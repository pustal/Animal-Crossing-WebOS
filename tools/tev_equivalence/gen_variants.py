#!/usr/bin/env python3
"""Writes random TEV variant sources (same const-block format as pc_gx_tev.c) for the equivalence test."""
import random
import sys
from pathlib import Path


def ivec(values):
    return f"ivec{len(values)}({','.join(str(v) for v in values)})"


def const_block(rng: random.Random) -> str:
    n = rng.randint(1, 3)
    ind = rng.random() < 0.2
    ksel_choices = list(range(0, 8)) + list(range(12, 32))
    lines = [f"const int u_num_tev_stages = {n};"]
    lines.append("const ivec4 u_tev_color_in[3] = ivec4[3](" + ",".join(ivec([rng.randint(0, 15) for _ in range(4)]) for _ in range(3)) + ");")
    lines.append("const ivec4 u_tev_alpha_in[3] = ivec4[3](" + ",".join(ivec([rng.randint(0, 7) for _ in range(4)]) for _ in range(3)) + ");")
    lines.append("const int u_tev_color_op[3] = int[3](%d,%d,%d);" % tuple(rng.randint(0, 1) for _ in range(3)))
    lines.append("const int u_tev_alpha_op[3] = int[3](%d,%d,%d);" % tuple(rng.randint(0, 1) for _ in range(3)))
    lines.append("const int u_tev_tc_src[3] = int[3](%d,%d,%d);" % tuple(rng.randint(0, 1) for _ in range(3)))
    lines.append("const ivec4 u_tev_bsc[3] = ivec4[3](" + ",".join(ivec([rng.randint(0, 2), rng.randint(0, 3), rng.randint(0, 2), rng.randint(0, 3)]) for _ in range(3)) + ");")
    lines.append("const ivec4 u_tev_out[3] = ivec4[3](" + ",".join(ivec([rng.randint(0, 1), rng.randint(0, 1), rng.randint(0, 3), rng.randint(0, 3)]) for _ in range(3)) + ");")
    lines.append("const ivec2 u_tev_swap[3] = ivec2[3](" + ",".join(ivec([rng.randint(0, 3), rng.randint(0, 3)]) for _ in range(3)) + ");")
    lines.append("const ivec3 u_tev_ksel[3] = ivec3[3](" + ",".join(ivec([rng.choice(ksel_choices), rng.choice(ksel_choices), i]) for i in range(3)) + ");")
    mtx = [0, 1, 2, 3, 5, 6, 7, 9, 10, 11]
    lines.append("const ivec4 u_tev_ind_cfg[3] = ivec4[3](" + ",".join(
        ivec([rng.randint(0, 1), rng.choice(mtx) if ind else 0, rng.randint(0, 7), 0]) for _ in range(3)) + ");")
    lines.append("const ivec3 u_tev_ind_wrap[3] = ivec3[3](" + ",".join(
        ivec([rng.randint(0, 6), rng.randint(0, 6), rng.randint(0, 1)]) for _ in range(3)) + ");")
    lines.append("const int u_use_texture[3] = int[3](%d,%d,%d);" % tuple(rng.randint(0, 1) for _ in range(3)))
    lines.append("const ivec4 u_lighting_cfg0 = ivec4(%d,%d,%d,%d);" % tuple(rng.randint(0, 1) for _ in range(4)))
    lines.append("const ivec4 u_lighting_cfg1 = ivec4(%d,%d,%d,0);" % (rng.randint(0, 1), rng.randint(0, 1), rng.randint(0, 255)))
    lines.append("const ivec4 u_swap_table[4] = ivec4[4](" + ",".join(ivec([rng.randint(0, 3) for _ in range(4)]) for _ in range(4)) + ");")
    lines.append("const ivec3 u_alpha_ctrl = ivec3(%d,%d,%d);" % (rng.choice([7, 7, rng.randint(0, 7)]), rng.randint(0, 3), rng.choice([7, 7, rng.randint(0, 7)])))
    lines.append(f"const int u_num_ind_stages = {1 if ind else 0};")
    lines.append(f"const int u_fog_enable = {rng.randint(0, 1)};")
    return "\n".join(lines) + "\n"


def main() -> None:
    template = Path(sys.argv[1]).read_text().splitlines(keepends=True)
    out_dir = Path(sys.argv[2])
    count = int(sys.argv[3])
    out_dir.mkdir(exist_ok=True)
    head = [l for l in template if not l.startswith("const ")]
    first_const = next(i for i, l in enumerate(template) if l.startswith("const "))
    rng = random.Random(1234)
    for i in range(count):
        body = "".join(template[:first_const]) + const_block(rng) + "".join(head[first_const:])
        (out_dir / f"v{i:04d}.frag").write_text(body)


if __name__ == "__main__":
    main()
