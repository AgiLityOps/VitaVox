/* Bottom hint bar layout (pure; host-tested). Hints are right-aligned in the
 * order added; each is one or two button glyphs then a label (00.33: two
 * glyphs, e.g. (L) (R) Skip). */
#ifndef VITAVOX_HINTLAYOUT_H
#define VITAVOX_HINTLAYOUT_H

#define HINT_LABEL_GAP 8.0f   /* glyph -> label */
#define HINT_GLYPH_GAP 4.0f   /* between the two glyphs */
#define HINT_ITEM_GAP 22.0f   /* between hints */

typedef struct {
    float label_w;       /* label ink width */
    float glyph_w[2];    /* glyph widths */
    int nglyphs;         /* 1 or 2 */
    /* out */
    float label_x;       /* left of the label ink */
    float glyph_cx[2];   /* glyph centres, left to right */
} HintItem;

/* Lays out n hints right-aligned ending at right; returns the left edge of the leftmost one. */
float hint_layout(HintItem *h, int n, float right);
/* Width of one hint (glyphs, gaps, label). */
float hint_width(const HintItem *h);

#endif
