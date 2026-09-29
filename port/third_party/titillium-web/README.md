# Titillium Web

The typeface of the settings overlay (`port/linux/src/overlay.c`), by the
Accademia di Belle Arti di Urbino, under the SIL Open Font License 1.1
(see `OFL.txt`).

Upstream: https://github.com/google/fonts/tree/main/ofl/titilliumweb.
`TitilliumWeb-SemiBold.ttf` and `OFL.txt` are copied unchanged.

The game does not read the font file. `tools/overlay_font.py` draws its
printable ASCII into a signed distance field atlas,
`port/linux/src/overlay_font.h`, which the Linux and Windows builds
compile in.
