#!/usr/bin/env python3
"""Regenerate include/themes_data.h from themes/*.ini"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
THEMES_DIR = ROOT / "themes"
OUT = ROOT / "include" / "themes_data.h"

MODE_KEYS = [
    ("Spectrum", 0),
    ("Stereometer", 1),
    ("Stereogram", 1),
    ("Spectrogram", 2),
    ("Waveform", 3),
    ("Oscilloscope", 4),
]


def parse_rgb(val):
    parts = [p.strip() for p in val.split(",")]
    if len(parts) < 3:
        return None
    try:
        r, g, b = int(parts[0]), int(parts[1]), int(parts[2])
        return (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))
    except ValueError:
        return None


def parse_ini(path):
    sections = {"": {}}
    cur = ""
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1].strip()
            sections.setdefault(cur, {})
            continue
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        sections[cur][k.strip()] = v.strip()
    return sections


def main():
    themes = []
    for path in sorted(THEMES_DIR.glob("*.ini")):
        if path.name.startswith("_"):
            continue
        sec = parse_ini(path)
        allc = sec.get("All", {})

        def need(key, default):
            if key not in allc:
                return default
            rgb = parse_rgb(allc[key])
            return rgb if rgb else default

        primary = need("primary", (172, 192, 222))
        secondary = need("secondary", (86, 96, 111))
        text = need("text", (255, 255, 255))
        bg = need("background", (0, 0, 0))
        bg2 = need("background_secondary", (43, 48, 55))
        overrides = {}
        for section_name, idx in MODE_KEYS:
            if section_name not in sec or "primary" not in sec[section_name]:
                continue
            rgb = parse_rgb(sec[section_name]["primary"])
            if rgb:
                overrides[idx] = rgb
        themes.append((path.stem, primary, secondary, text, bg, bg2, overrides))

    names = [t[0] for t in themes]
    default_idx = names.index("Direct") if "Direct" in names else 0

    def rgb(x):
        return f"{{{x[0]}, {x[1]}, {x[2]}}}"

    lines = [
        "#pragma once",
        "// Generated from themes/*.ini — re-run: python3 tools/gen_themes.py",
        "#include <stdint.h>",
        "",
        "struct ThemeRgb {",
        "  uint8_t r;",
        "  uint8_t g;",
        "  uint8_t b;",
        "};",
        "",
        "struct ThemeDef {",
        "  const char *name;",
        "  ThemeRgb primary;",
        "  ThemeRgb secondary;",
        "  ThemeRgb text;",
        "  ThemeRgb background;",
        "  ThemeRgb background_secondary;",
        "  uint8_t mode_primary_mask;  // bits 0..4 for viz modes",
        "  ThemeRgb mode_primary[5];",
        "};",
        "",
        f"static const uint8_t kThemeCount = {len(themes)};",
        f"static const uint8_t kThemeDefault = {default_idx};",
        "",
        "static const ThemeDef kThemes[kThemeCount] = {",
    ]
    for name, p, s, t, bg, bg2, ov in themes:
        mask = 0
        mp = [(0, 0, 0)] * 5
        for idx, color in ov.items():
            mask |= 1 << idx
            mp[idx] = color
        mp_str = ", ".join(rgb(x) for x in mp)
        cname = name.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(
            f'  {{"{cname}", {rgb(p)}, {rgb(s)}, {rgb(t)}, {rgb(bg)}, {rgb(bg2)}, '
            f"0x{mask:02x}, {{{mp_str}}}}},"
        )
    lines.append("};")
    lines.append("")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(themes)} themes, default={themes[default_idx][0]})")


if __name__ == "__main__":
    main()
