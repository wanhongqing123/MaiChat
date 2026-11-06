#!/usr/bin/env python3
"""Generate native presentation constants from the shared typography contract."""
import argparse
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
spec = json.loads((root / "MaiChat/Presentation/typography.json").read_text())
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
banner = "// 自动生成自 MaiChat/Presentation/typography.json；修改定义后运行 generate-typography.py。\n"
roles, colors, spacing = spec["roles"], spec["colors"], spec["spacing"]
dark_colors = spec["dark_colors"]

swift = banner + "import Foundation\n\npublic enum AppTextRole: String, CaseIterable, Sendable {\n"
swift += "    case " + ", ".join(roles) + "\n\n    public var points: Double {\n        switch self {\n"
swift += "".join(f"        case .{k}: return {v * spec['mobile_body']:.3f}\n" for k, v in roles.items())
swift += "        }\n    }\n}\n\npublic enum AppTextSize: String, CaseIterable, Sendable {\n"
swift += "    case system, standard, large, extraLarge\n    public func scale(systemScale: Double = 1) -> Double {\n        switch self {\n"
swift += "        case .system: return systemScale.isFinite && systemScale > 0 ? systemScale : 1\n"
swift += "".join(f"        case .{k}: return {v}\n" for k, v in spec["presets"].items())
swift += "        }\n    }\n}\n\npublic enum AppTypographyTokens {\n"
swift += f"    public static let defaultPreset = AppTextSize.{spec['default_preset']}\n"
swift += '    public static let preferenceKey = "maichat.preferences.text-size"\n'
swift += "    public static func heading(_ level: Int) -> AppTextRole {\n        switch level {\n        case 1: return .heading1\n        case 2: return .heading2\n        case 3: return .heading3\n        default: return .body\n        }\n    }\n"
swift += "".join(f'    public static let {k}Color: UInt32 = 0x{v[1:]}\n' for k, v in colors.items())
swift += "".join(f'    public static let {k}DarkColor: UInt32 = 0x{v[1:]}\n' for k, v in dark_colors.items())
swift += "".join(f"    public static let {k}Ratio: Double = {v}\n" for k, v in spacing.items()) + "}\n"

java = banner + "package com.kongshang.maichat;\n\npublic final class MaiChatTypographyTokens {\n    private MaiChatTypographyTokens() {}\n"
java += "    public enum Role {\n        " + ", ".join(f"{k.upper()}({v * spec['mobile_body']:.3f}f)" for k, v in roles.items()) + ";\n"
java += "        public final float points;\n        Role(float points) { this.points = points; }\n    }\n"
java += '    public static final String DEFAULT_PRESET = "' + spec["default_preset"] + '";\n'
java += "    public static float scale(String preset, float systemScale) {\n        switch (preset) {\n"
java += '            case "system": return Float.isFinite(systemScale) && systemScale > 0 ? systemScale : 1;\n'
java += "".join(f'            case "{k}": return {v}f;\n' for k, v in spec["presets"].items())
java += "            default: return 1;\n        }\n    }\n"
java += "    public static float headingRatio(int level) {\n        return level == 1 ? " + str(roles["heading1"]) + "f : level == 2 ? " + str(roles["heading2"]) + "f : level == 3 ? " + str(roles["heading3"]) + "f : 1f;\n    }\n"
java += "".join(f'    public static final int {k.upper()}_COLOR = 0xff{v[1:]};\n' for k, v in colors.items())
java += "".join(f'    public static final int {k.upper()}_DARK_COLOR = 0xff{v[1:]};\n' for k, v in dark_colors.items())
java += "".join(f"    public static final float {k.upper()}_RATIO = {v}f;\n" for k, v in spacing.items()) + "}\n"

cpp = banner + "#pragma once\n\nnamespace TypographyTokens {\n"
cpp += f"inline constexpr double kBody = {spec['desktop_body']}.0;\n"
cpp += "".join(f"inline constexpr double k{k[0].upper() + k[1:]}Scale = {v};\n" for k, v in spec["presets"].items())
cpp += "".join(f"inline constexpr double k{k[0].upper() + k[1:]}Ratio = {v};\n" for k, v in roles.items())
cpp += "".join(f'inline constexpr const char* k{k[0].upper() + k[1:]}Color = "{v}";\n' for k, v in colors.items())
cpp += "".join(f'inline constexpr const char* k{k[0].upper() + k[1:]}DarkColor = "{v}";\n' for k, v in dark_colors.items())
cpp += "".join(f"inline constexpr double k{k[0].upper() + k[1:]}Ratio = {v};\n" for k, v in spacing.items())
cpp += "}  // namespace TypographyTokens\n"

# Font names and choices are data shared by native platform adapters.
fonts = spec["font_families"]
quote = lambda value: json.dumps(value, ensure_ascii=False)
swift += "\npublic enum AppFontFamily: String, CaseIterable, Sendable {\n"
swift += "    case " + ", ".join(f["id"] for f in fonts) + "\n"
for name, key in [("title", "title"), ("familyName", "family"), ("regularName", "regular"), ("mediumName", "medium"), ("semiboldName", "semibold"), ("boldName", "bold")]:
    swift += "    public var " + name + ": String {\n        switch self {\n"
    swift += "".join("        case ." + f["id"] + ": return " + quote(f[key]) + "\n" for f in fonts)
    swift += "        }\n    }\n"
swift += "    public var windowsOnly: Bool { self == .microsoftYaHei || self == .simSun }\n}\n"
swift += "public enum AppFontFamilyTokens {\n    public static let preferenceKey = " + quote(spec["font_family_preference_key"]) + "\n"
swift += "    public static let defaultFamily = AppFontFamily." + spec["default_font_family"] + "\n}\n"
java += "\n" + "final class MaiChatFontFamilyTokens {\n    private MaiChatFontFamilyTokens() {}\n"
java += "    static final String PREFERENCE_KEY = " + quote(spec["font_family_preference_key"]) + ";\n"
java += "    enum Family {\n        " + ",\n        ".join(f["id"].upper() + "(" + ", ".join(quote(f[k]) for k in ["id", "title", "family", "regular", "medium", "semibold", "bold"]) + ", " + str(f.get("windows_only", False)).lower() + ")" for f in fonts) + ";\n"
java += "        final String id, title, family, regular, medium, semibold, bold;\n        final boolean windowsOnly;\n"
java += "        Family(String id, String title, String family, String regular, String medium, String semibold, String bold, boolean windowsOnly) {\n            this.id = id; this.title = title; this.family = family; this.regular = regular; this.medium = medium; this.semibold = semibold; this.bold = bold; this.windowsOnly = windowsOnly;\n        }\n"
java += "        static Family fromId(String id) { for (Family family : values()) if (family.id.equals(id)) return family; return SYSTEM; }\n    }\n}\n"
cpp += "\nnamespace TypographyTokens {\nstruct FontFamily {\n    const char* id;\n    const char* title;\n    const char* family;\n    const char* regular;\n    const char* medium;\n    const char* semibold;\n    const char* bold;\n    bool windowsOnly;\n};\n"
cpp += "inline constexpr const char* kFontFamilyPreferenceKey = " + quote(spec["font_family_preference_key"]) + ";\n"
cpp += "inline constexpr FontFamily kFontFamilies[] = {\n"
cpp += "".join("    {" + ", ".join(("u8" if k == "title" else "") + quote(f[k]) for k in ["id", "title", "family", "regular", "medium", "semibold", "bold"]) + ", " + str(f.get("windows_only", False)).lower() + "},\n" for f in fonts)
cpp += "};\n}  // namespace TypographyTokens\n"

outputs = {
    "MaiChat/ios/MaiChat/MaiChatCore/Chat/AppTypographyTokens.swift": swift,
    "MaiChat/android/MaiChat/app/src/main/java/ui/com/kongshang/maichat/MaiChatTypographyTokens.java": java,
    "MaiChat/desktop/src/markdown/TypographyTokens.h": cpp,
}
stale = []
for name, text in outputs.items():
    path = root / name
    if args.check:
        if not path.exists() or path.read_text() != text:
            stale.append(name)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
if stale:
    raise SystemExit("Regenerate typography constants: " + ", ".join(stale))
print("Typography constants verified" if args.check else "Typography constants generated")
