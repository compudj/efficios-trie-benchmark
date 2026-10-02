"""
dcache_plotlib -- what every dcache figure does the same way with a sweep CSV.

The sweeps pace their writers (bench --rename-rate / --churn-rate) wherever a
figure compares READERS across engines, and mark each row with whether the
writers sustained the offered rate (column `paced`: OK, SHORT, or - when
unpaced).  A SHORT row is a result -- that engine's writers cannot carry the
load -- but its reader number was measured under a LIGHTER load than the
other engines' at the same x, so it must not sit on the curve as if it were
comparable.  series() splits the two and plot_series() draws the paced points
as the curve and the SHORT ones as hollow, unconnected markers.
"""
import csv

from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator
from mpl_toolkits.axes_grid1 import make_axes_locatable


def load(path):
    return list(csv.DictReader(open(path)))


def series(rows, x, y, **match):
    """-> (xs, ys, short_xs, short_ys) over conserved rows whose columns equal
    **match (compared as strings).  Rows without a `paced` column count as
    paced."""
    ok, short = {}, {}
    want = {k: str(v) for k, v in match.items()}
    for r in rows:
        if r.get("conserved") != "OK":
            continue
        if any(r.get(k) != v for k, v in want.items()):
            continue
        yv = float(r[y])
        if yv <= 0:
            continue
        (short if r.get("paced") == "SHORT" else ok)[float(r[x])] = yv
    xs, sx = sorted(ok), sorted(short)
    return xs, [ok[v] for v in xs], sx, [short[v] for v in sx]


def plot_series(ax, s, color, marker, label, lw=2.0, ms=6.5, alpha=0.94):
    """Draw a series() result: the paced curve, then the SHORT points hollow."""
    xs, ys, sx, sy = s
    if xs:
        ax.plot(xs, ys, color=color, marker=marker, lw=lw, ms=ms, label=label,
                alpha=alpha)
    if sx:
        ax.plot(sx, sy, ls="none", marker=marker, ms=ms, mfc="none",
                mec=color, mew=1.4, alpha=0.8,
                label=None if xs else label)
    return bool(xs or sx)


def short_handle():
    """A legend entry explaining the hollow markers."""
    return Line2D([], [], ls="none", marker="o", mfc="none", mec="0.35",
                  mew=1.4, ms=6.5,
                  label="hollow: writers could not sustain the offered\n"
                        "rate (reader measured under a lighter load)")


def legend(ax, **kw):
    """ax.legend() plus the hollow-marker entry when the axes has SHORT points."""
    h, l = ax.get_legend_handles_labels()
    if any(getattr(line, "get_mfc", lambda: None)() == "none"
           for line in ax.get_lines()):
        h, l = h + [short_handle()], l + [short_handle().get_label()]
    if h:
        ax.legend(h, l, **kw)


def rate_label(v):
    """10000 -> '10k', 1000000 -> '1M' (plain labels, never 10^n)."""
    if v >= 1e6:
        return f"{v / 1e6:g}M"
    if v >= 1e3:
        return f"{v / 1e3:g}k"
    return f"{v:g}"


# Ratio ticks for ratio_strip, picked by how widely the engines spread: fine
# near parity, doubling when they spread over an order of magnitude.
_RATIO_VFINE = (0.7, 0.8, 0.9, 1, 1.1, 1.25, 1.4)
_RATIO_FINE = (0.5, 0.75, 1, 1.25, 1.5, 2)
_RATIO_MEDIUM = (0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4)
_RATIO_COARSE = (0.0625, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16)


def ratio_strip(ax, rows, x, y, engines, color, marker, base="seqlock",
                size="34%", **match):
    """Under @ax, a strip of EVERY engine's @y divided by @base's at the same x.

    No threshold and no favourite: wins, ties and losses of every arm, on a log2
    axis so 2x and 0.5x are the same distance from the 1.0 line.  Paced points
    only -- a SHORT point was measured under a lighter load than its neighbours,
    so it cannot be either side of a ratio (the curve simply has a gap there).
    Moves @ax's x label onto the strip.  Returns the strip's axes.
    """
    rax = make_axes_locatable(ax).append_axes("bottom", size=size, pad=0.08,
                                              sharex=ax)
    b = dict(zip(*series(rows, x, y, engine=base, **match)[:2]))
    lo = hi = 1.0
    lines = []
    for e in engines:
        if e == base:
            continue
        s = dict(zip(*series(rows, x, y, engine=e, **match)[:2]))
        xs = [v for v in sorted(s) if v in b and b[v] > 0]
        if not xs:
            continue
        rs = [s[v] / b[v] for v in xs]
        lo, hi = min(lo, *rs), max(hi, *rs)
        lines.append((e, xs, rs))
    # Arms that measure the same (identical reader code, e.g. every txn arm's
    # readdir) coincide to within a pixel on this axis, and the last one drawn
    # used to hide the rest.  Same thin line for every arm (no emphasis), but
    # HOLLOW markers, each a size smaller than the one drawn before it, so
    # coinciding arms show as nested outlines of their own shapes and colours.
    n = len(lines)
    for i, (e, xs, rs) in enumerate(lines):
        k = n - 1 - i			# 0 for the last (topmost) line
        rax.plot(xs, rs, color=color[e], marker=marker[e], lw=1.2,
                 ms=4.0 + 1.5 * k, mfc="none", mec=color[e], mew=1.3,
                 alpha=0.95)
    rax.axhline(1.0, color="0.3", lw=1.0, ls="--", zorder=0)
    rax.set_yscale("log", base=2)
    rax.set_ylim(lo / 1.15, hi * 1.15)
    ticks = (_RATIO_VFINE if hi / lo < 1.6 else
             _RATIO_FINE if hi / lo < 2.6 else
             _RATIO_MEDIUM if hi / lo < 8 else _RATIO_COARSE)
    ticks = [t for t in ticks if lo / 1.15 <= t <= hi * 1.15]
    rax.yaxis.set_major_locator(FixedLocator(ticks))
    rax.yaxis.set_major_formatter(FuncFormatter(lambda v, p: f"{v:g}×"))
    rax.yaxis.set_minor_locator(NullLocator())
    rax.tick_params(axis="y", labelsize=8)
    rax.grid(alpha=0.3, ls=":")
    rax.set_ylabel(f"÷ {base}", fontsize=8.5)
    rax.set_xlabel(ax.get_xlabel())
    ax.set_xlabel("")
    ax.tick_params(labelbottom=False)
    return rax

