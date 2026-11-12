"""An original DS-era menu look for the patcher, drawn as whole pixels on a 256x192 screen.

Everything here is pure data and arithmetic, so it is testable without Tk: an original
5x7 pixel font with descenders, word wrapping, paging, and framed panels. Nothing is
taken from any game; the window scales these pixels up by a whole number.
"""

SCREEN = (256, 192)
LINE = 9  # glyph rows: 7 above the baseline, 2 for descenders
SPACING = 1

PALETTE = {
    "stripe_a": "#b9d3ea", "stripe_b": "#a9c6e2",
    "title": "#27354d", "title_text": "#f7f7f2", "title_shadow": "#0e1626",
    "outline": "#2c3444", "border": "#5a7cb4", "inner": "#cfdcf1", "fill": "#fbfbf6",
    "message_border": "#4b6a92",
    "text": "#3f4146", "shadow": "#cdced3", "dim": "#a4a6ad", "dim_shadow": "#e6e7ea",
    "label": "#3d64a6", "error": "#b3352c", "done": "#2f7d3a", "cursor": "#3f4146",
}

_GLYPHS = """
A .###. #...# #...# ##### #...# #...# #...#
B ####. #...# #...# ####. #...# #...# ####.
C .###. #...# #.... #.... #.... #...# .###.
D ####. #...# #...# #...# #...# #...# ####.
E ##### #.... #.... ####. #.... #.... #####
F ##### #.... #.... ####. #.... #.... #....
G .###. #...# #.... #.### #...# #...# .####
H #...# #...# #...# ##### #...# #...# #...#
I ### .#. .#. .#. .#. .#. ###
J ..### ...#. ...#. ...#. #..#. #..#. .##..
K #...# #..#. #.#.. ##... #.#.. #..#. #...#
L #.... #.... #.... #.... #.... #.... #####
M #...# ##.## #.#.# #.#.# #...# #...# #...#
N #...# ##..# #.#.# #..## #...# #...# #...#
O .###. #...# #...# #...# #...# #...# .###.
P ####. #...# #...# ####. #.... #.... #....
Q .###. #...# #...# #...# #.#.# #..#. .##.#
R ####. #...# #...# ####. #.#.. #..#. #...#
S .#### #.... #.... .###. ....# ....# ####.
T ##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..
U #...# #...# #...# #...# #...# #...# .###.
V #...# #...# #...# #...# #...# .#.#. ..#..
W #...# #...# #...# #.#.# #.#.# #.#.# .#.#.
X #...# #...# .#.#. ..#.. .#.#. #...# #...#
Y #...# #...# .#.#. ..#.. ..#.. ..#.. ..#..
Z ##### ....# ...#. ..#.. .#... #.... #####
a ..... ..... .###. ....# .#### #...# .####
b #.... #.... ####. #...# #...# #...# ####.
c ..... ..... .###. #.... #.... #.... .###.
d ....# ....# .#### #...# #...# #...# .####
e ..... ..... .###. #...# ##### #.... .###.
é ...#. ..#.. .###. #...# ##### #.... .###.
f ..## .#.. #### .#.. .#.. .#.. .#..
g ..... ..... .#### #...# #...# #...# .#### ....# .###.
h #.... #.... ####. #...# #...# #...# #...#
i # . # # # # #
j ..# ... ..# ..# ..# ..# ..# #.# .#.
k #... #... #..# #.#. ##.. #.#. #..#
l # # # # # # #
m ..... ..... ##.#. #.#.# #.#.# #.#.# #.#.#
n ..... ..... ####. #...# #...# #...# #...#
o ..... ..... .###. #...# #...# #...# .###.
p ..... ..... ####. #...# #...# #...# ####. #.... #....
q ..... ..... .#### #...# #...# #...# .#### ....# ....#
r .... .... #.## ##.. #... #... #...
s ..... ..... .#### #.... .###. ....# ####.
t .#.. .#.. #### .#.. .#.. .#.. ..##
u ..... ..... #...# #...# #...# #...# .####
v ..... ..... #...# #...# #...# .#.#. ..#..
w ..... ..... #...# #...# #.#.# #.#.# .#.#.
x ..... ..... #...# .#.#. ..#.. .#.#. #...#
y ..... ..... #...# #...# #...# #...# .#### ....# .###.
z ..... ..... ##### ...#. ..#.. .#... #####
0 .###. #...# #..## #.#.# ##..# #...# .###.
1 .#. ##. .#. .#. .#. .#. ###
2 .###. #...# ....# ...#. ..#.. .#... #####
3 ####. ....# ....# .###. ....# ....# ####.
4 ...#. ..##. .#.#. #..#. ##### ...#. ...#.
5 ##### #.... ####. ....# ....# #...# .###.
6 .###. #.... #.... ####. #...# #...# .###.
7 ##### ....# ...#. ..#.. .#... .#... .#...
8 .###. #...# #...# .###. #...# #...# .###.
9 .###. #...# #...# .#### ....# ....# .###.
. . . . . . . #
, .. .. .. .. .. .# .# #.
: . . # . . # .
; .. .. .# .. .. .# #.
! # # # # # . #
? .###. #...# ....# ...#. ..#.. ..... ..#..
' # # . . . . .
’ # # . . . . .
" #.# #.# ... ... ... ... ...
- .... .... .... #### .... .... ....
_ ..... ..... ..... ..... ..... ..... ..... #####
( ..# .#. #.. #.. #.. .#. ..#
) #.. .#. ..# ..# ..# .#. #..
[ ### #.. #.. #.. #.. #.. ###
] ### ..# ..# ..# ..# ..# ###
/ ....# ...#. ...#. ..#.. .#... .#... #....
\\ #.... .#... .#... ..#.. ...#. ...#. ....#
+ ..... ..#.. ..#.. ##### ..#.. ..#.. .....
= .... .... #### .... #### .... ....
% ##..# ##.#. ...#. ..#.. .#... .#.## #..##
& .##.. #..#. #.#.. .#... #.#.# #..#. .##.#
~ ..... ..... .#..# #.##. ..... ..... .....
* ..... #.#.# .###. ##### .###. #.#.# .....
# .#.#. ##### .#.#. .#.#. .#.#. ##### .#.#.
@ .###. #...# #.### #.#.# #.### #.... .###.
< ...# ..#. .#.. #... .#.. ..#. ...#
> #... .#.. ..#. ...# ..#. .#.. #...
""".strip().splitlines()

FONT = {}
for _line in _GLYPHS:
    _char, *_rows = _line.split(" ")
    _char = _char.replace("\\\\", "\\")
    FONT[_char] = tuple(_rows + ["." * len(_rows[0])] * (LINE - len(_rows)))
FONT[" "] = ("...",) * LINE
# Anything the font lacks draws as a hollow box rather than vanishing.
MISSING = ("####",) + ("#..#",) * 5 + ("####",) + ("....",) * 2


def glyph(char):
    return FONT.get(char, MISSING)


def measure(text):
    return sum(len(glyph(char)[0]) + SPACING for char in text) - (SPACING if text else 0)


def wrap(text, width):
    """Break text into lines no wider than width, splitting overlong words such as paths."""
    lines = []
    for paragraph in text.split("\n"):
        line = ""
        for word in paragraph.split(" "):
            candidate = f"{line} {word}" if line else word
            if measure(candidate) <= width:
                line = candidate
                continue
            if line:
                lines.append(line)
            line = ""
            while measure(word) > width:
                cut = max(index for index in range(1, len(word) + 1) if measure(word[:index]) <= width)
                lines.append(word[:cut])
                word = word[cut:]
            line = word
        lines.append(line)
    return lines


def pages(text, width, per_page=2):
    """Group wrapped lines into message-box pages; blank lines only separate paragraphs."""
    lines = wrap(text, width)
    result, page = [], []
    for line in lines:
        if not line:
            if page:
                result.append(page)
                page = []
            continue
        page.append(line)
        if len(page) == per_page:
            result.append(page)
            page = []
    if page:
        result.append(page)
    return result or [[""]]


def text_runs(text, x, y):
    """Return (x, y, width) horizontal pixel runs, each one pixel tall, for text at x, y."""
    runs = []
    for char in text:
        rows = glyph(char)
        for row_index, row in enumerate(rows):
            start = None
            for column, pixel in enumerate(row + "."):
                if pixel == "#" and start is None:
                    start = column
                elif pixel != "#" and start is not None:
                    runs.append((x + start, y + row_index, column - start))
                    start = None
        x += len(rows[0]) + SPACING
    return runs


def frame(x, y, width, height, border):
    """Rectangles (x, y, w, h, colour key) for a rounded, outlined, double-bordered panel."""
    right, bottom = x + width, y + height
    return [
        # Dark outline with the corner pixels cut, which rounds each corner.
        (x + 2, y, width - 4, 1, "outline"), (x + 2, bottom - 1, width - 4, 1, "outline"),
        (x, y + 2, 1, height - 4, "outline"), (right - 1, y + 2, 1, height - 4, "outline"),
        (x + 1, y + 1, 1, 1, "outline"), (right - 2, y + 1, 1, 1, "outline"),
        (x + 1, bottom - 2, 1, 1, "outline"), (right - 2, bottom - 2, 1, 1, "outline"),
        (x + 2, y + 1, width - 4, 2, border), (x + 2, bottom - 3, width - 4, 2, border),
        (x + 1, y + 2, 2, height - 4, border), (right - 3, y + 2, 2, height - 4, border),
        (x + 3, y + 3, width - 6, 1, "inner"), (x + 3, bottom - 4, width - 6, 1, "inner"),
        (x + 3, y + 3, 1, height - 6, "inner"), (right - 4, y + 3, 1, height - 6, "inner"),
        (x + 4, y + 4, width - 8, height - 8, "fill"),
    ]


def cursor(x, y):
    """A right-pointing selection arrow, seven pixels tall."""
    return [(x, y + row, min(row, 6 - row) + 1) for row in range(7)]


def more_arrow(x, y):
    """A small downward arrow shown when a message has another page."""
    return [(x + row, y + row, 7 - 2 * row) for row in range(4)]
