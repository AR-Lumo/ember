# Brand assets

| File | Use |
|---|---|
| `cinder-mark.svg` | The mark alone. App icons, favicons, anywhere the name is already present. |
| `cinder-wordmark.svg` | Mark and name, dark ink — for light backgrounds. |
| `cinder-wordmark-dark.svg` | The same, light ink — for dark backgrounds. |
| `cinder-file-icon.svg` | File-type icon for `.ci` source files. |

**The mark** is a chip of coal split open and still burning: an irregular
dark body, molten light through the break, and a splinter lying across
it. A cinder is what is left of a fire rather than the fire itself, so it
is deliberately not a flame — two earlier attempts were a flame, which
reads as any of a hundred fire icons, and a faceted solid, which read as
a box.

**The wordmark needs two files** because an SVG embedded with `<img>`
does not inherit `currentColor` from the page. A single file using it
renders black on black in a dark README. Select between them with
`<picture>`:

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/cinder-wordmark-dark.svg">
  <img src="assets/cinder-wordmark.svg" alt="Cinder" width="230">
</picture>
```

**Colours.** Coal `#26140f`, ember `#f97316`, heat `#fde047`, core
`#fffbeb`. The gradients carry the rest.

Everything here is MIT-licensed along with the rest of the project.
