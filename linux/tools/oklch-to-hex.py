#!/usr/bin/env python3
"""oklch -> sRGB #rrggbb converter.

Standalone — no numpy. Plug in the palette from linux/plans/ui-refactor-2026.md
and re-emit the hex table when the designer ships new oklch values.

Usage:
    ./tools/oklch-to-hex.py 0.72 0.14 220
or import the function:
    from oklch_to_hex import oklch_to_hex
"""
import math
import sys


def oklch_to_hex(L: float, C: float, h_deg: float) -> str:
    """Convert oklch(L C h) to a sRGB hex string. Clips out-of-gamut."""
    # 1) oklch -> oklab
    h_rad = math.radians(h_deg)
    a = C * math.cos(h_rad)
    b = C * math.sin(h_rad)

    # 2) oklab -> linear sRGB (Bjorn Ottosson's matrices)
    l_ = L + 0.3963377774 * a + 0.2158037573 * b
    m_ = L - 0.1055613458 * a - 0.0638541728 * b
    s_ = L - 0.0894841775 * a - 1.2914855480 * b
    l = l_ ** 3
    m = m_ ** 3
    s = s_ ** 3

    r_lin = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    g_lin = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    b_lin = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s

    # 3) linear -> sRGB gamma
    def to_srgb(x):
        x = max(0.0, min(1.0, x))
        if x <= 0.0031308:
            return 12.92 * x
        return 1.055 * (x ** (1.0 / 2.4)) - 0.055

    r = to_srgb(r_lin)
    g = to_srgb(g_lin)
    b = to_srgb(b_lin)

    return "#{:02x}{:02x}{:02x}".format(
        round(r * 255), round(g * 255), round(b * 255)
    )


# Reference table — keep aligned with linux/plans/ui-refactor-2026.md.
PALETTE = [
    ("--gx-bg-0",     0.16,  0.012, 250),
    ("--gx-bg-1",     0.195, 0.012, 250),
    ("--gx-bg-2",     0.225, 0.013, 250),
    ("--gx-bg-3",     0.26,  0.014, 250),
    ("--gx-bg-4",     0.31,  0.015, 250),
    ("--gx-border",   0.32,  0.012, 250),
    ("--gx-border-2", 0.39,  0.012, 250),
    ("--gx-text",     0.93,  0.005, 250),
    ("--gx-text-2",   0.78,  0.008, 250),
    ("--gx-muted",    0.60,  0.008, 250),
    ("--gx-faint",    0.46,  0.008, 250),
    ("--gx-accent",   0.72,  0.14,  220),
    ("--gx-accent-2", 0.64,  0.16,  220),
    ("--gx-tk-kw",    0.78,  0.14,  320),
    ("--gx-tk-fn",    0.82,  0.11,  195),
    ("--gx-tk-str",   0.82,  0.13,  145),
    ("--gx-tk-num",   0.82,  0.13,  70),
    ("--gx-tk-com",   0.52,  0.012, 250),
]


def emit_table():
    width = max(len(name) for name, *_ in PALETTE)
    print(f"{'token'.ljust(width)}  oklch                hex")
    print("-" * (width + 30))
    for name, L, C, h in PALETTE:
        oklch = f"{L} {C} {h}"
        hexv = oklch_to_hex(L, C, h)
        print(f"{name.ljust(width)}  {oklch.ljust(20)} {hexv}")


if __name__ == "__main__":
    if len(sys.argv) == 4:
        L, C, h = (float(x) for x in sys.argv[1:])
        print(oklch_to_hex(L, C, h))
    else:
        emit_table()
