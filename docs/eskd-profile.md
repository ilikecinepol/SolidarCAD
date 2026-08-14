# Russian ESKD drawing profile

The drawing layer is deliberately independent from the sketch editor. Geometry
is stored in millimetres and rendered through one vector path for the screen,
PDF and printer, so output does not depend on monitor resolution.

## Standards baseline

- GOST 2.301-68: A4 page size (210 × 297 mm)
- GOST 2.104-2006: frame and form-1 title-block placement
- GOST 2.303-68: semantic line classes and relative widths
- GOST 2.304-81: drawing-font metrics and preferred `GOST type B` family
- GOST 2.307-2011, including the 2021 correction: dimension presentation

## Implemented in the first slice

- portrait A4 page with 20 mm left and 5 mm remaining frame margins
- 185 × 55 mm title-block area in the lower-right corner
- separate thick visible outlines and thin dimension/extension lines
- closed rectangular sketch with horizontal and vertical dimensions
- PDF export and native Qt print dialog from the same renderer

## Conformance boundary

This is an engineering baseline, not a claim of certification. Before production
release we must validate every title-block cell against the normative form,
implement all dimension cases, add tolerances and surface symbols, and bundle a
redistributable GOST 2.304 stroke font. The renderer currently requests
`GOST type B` and uses a metrically similar system fallback when it is absent.
