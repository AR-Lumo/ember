# Brand assets

| File | Use |
|---|---|
| `soliton-mark.svg` | The mark alone. App icons, favicons, anywhere the name is already present. |
| `soliton-wordmark.svg` | Mark and name, dark ink — for light backgrounds. |
| `soliton-wordmark-dark.svg` | The same, light ink — for dark backgrounds. |
| `soliton-file-icon.svg` | File-type icon for `.sn` source files. |
| `og-card.html` | Source for the social card; renders to `docs/og-image.png`. |

**The mark is the equation.** A soliton is a solitary wave that keeps
its shape as it travels, and the canonical solution to the KdV equation
is a sech² pulse — so the silhouette is that curve, sampled at 96 points
rather than approximated with béziers. Two copies of the same pulse, the
trailing one fading off the left edge: identical shape at two positions
is what "does not disperse" looks like, and it is the whole idea in one
image.

The fade is not decoration. The echo is cut by the frame, and without it
that cut is a hard vertical wall that reads as a rendering fault.

**These are generated, not drawn.** `make_mark.py` emits the mark from
the equation; `make_assets.py` derives the wordmark and file icon from
it. Change the curve in one place and re-run both, rather than editing
four files and hoping they still agree.

**The wordmark ships twice** because an SVG embedded with `<img>` does
not inherit `currentColor` from the page. A single file using it renders
invisible in one theme. Select between them:

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/soliton-wordmark-dark.svg">
  <img src="assets/soliton-wordmark.svg" alt="Soliton" width="250">
</picture>
```

**Colours.** Ink `#0b1620`, deep `#0e7490`, pulse `#22d3ee`, crest
`#7dd3fc`. The gradients carry the rest.

## The social card

`og-card.html` is the source; `docs/og-image.png` is the render the site
serves. Rebuild it after editing the card:

```bash
chrome --headless=new --disable-gpu --hide-scrollbars \
       --force-device-scale-factor=1 --window-size=1200,630 \
       --virtual-time-budget=6000 \
       --screenshot=docs/og-image.png assets/og-card.html
```

Two things about that command are load-bearing.
`--virtual-time-budget` is not optional: without it the shot is taken
before the webfonts arrive and the card renders in Times. And **both
paths must be absolute** — given a relative `--screenshot` target and a
relative page, headless Chrome exits successfully having written
nothing, leaving the previous render in place. It looks like it worked.
Check the mtime.

It is a PNG rather than the SVG mark because most platforms will not
render an SVG in a link preview, and a broken preview is worse than
none. 1200×630 is the size Open Graph, Slack and Twitter all agree on.

Everything here is MIT-licensed along with the rest of the project.
