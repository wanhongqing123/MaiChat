#!/usr/bin/env python3
"""Write and verify the fixed MaiChat Finder drag-to-Applications layout."""

import argparse
from pathlib import Path

from ds_store import DSStore


WINDOW = {
    "WindowBounds": "{{120, 120}, {580, 380}}",
    "ShowStatusBar": False,
    "ShowTabView": False,
    "ShowToolbar": False,
    "ShowPathbar": False,
    "ShowSidebar": False,
    "ContainerShowSidebar": False,
    "PreviewPaneVisibility": False,
    "SidebarWidth": 180,
}
ICONS = {
    "viewOptionsVersion": 1,
    "backgroundType": 1,
    "backgroundColorRed": 1.0,
    "backgroundColorGreen": 1.0,
    "backgroundColorBlue": 1.0,
    "gridOffsetX": 0.0,
    "gridOffsetY": 0.0,
    "gridSpacing": 100.0,
    "arrangeBy": "none",
    "showIconPreview": True,
    "showItemInfo": False,
    "labelOnBottom": True,
    "textSize": 13.0,
    "iconSize": 96.0,
    "scrollPositionX": 0.0,
    "scrollPositionY": 0.0,
}
POSITIONS = {"MaiChat.app": (150, 165), "Applications": (430, 165)}


def verify(path: Path) -> None:
    # Close before reopening: DSStore buffers writes until flush/close. Verification
    # also catches incomplete metadata before hdiutil turns the image read-only.
    with DSStore.open(str(path), "r") as store:
        if store["."]["bwsp"] != WINDOW or store["."]["icvp"] != ICONS:
            raise ValueError("Finder window or icon settings did not round-trip")
        if store["."]["icvl"] != (b"type", b"icnv"):
            raise ValueError("Finder default view is not icon view")
        for name, position in POSITIONS.items():
            if store[name]["Iloc"] != position:
                raise ValueError(f"Finder icon position is invalid: {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("volume", type=Path, help="Mounted writable MaiChat image")
    parser.add_argument("--verify-only", action="store_true")
    arguments = parser.parse_args()
    volume = arguments.volume
    if not (volume / "MaiChat.app").is_dir():
        parser.error("volume must contain MaiChat.app")
    applications = volume / "Applications"
    if not applications.is_symlink() or applications.readlink() != Path("/Applications"):
        parser.error("volume must contain an Applications symlink to /Applications")
    path = volume / ".DS_Store"
    if not arguments.verify_only:
        with DSStore.open(str(path), "w+") as store:
            store["."]["vSrn"] = ("long", 1)
            store["."]["bwsp"] = WINDOW
            store["."]["icvp"] = ICONS
            store["."]["icvl"] = (b"type", b"icnv")
            for name, position in POSITIONS.items():
                store[name]["Iloc"] = position
    verify(path)
    print("MaiChat Finder layout verified: 580x380, app -> Applications")


if __name__ == "__main__":
    main()
