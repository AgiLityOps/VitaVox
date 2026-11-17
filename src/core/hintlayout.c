#include "hintlayout.h"

float hint_width(const HintItem *h) {
    int ng = h->nglyphs < 1 ? 1 : h->nglyphs > 2 ? 2 : h->nglyphs;
    float w = h->label_w + HINT_LABEL_GAP + h->glyph_w[0];
    if (ng == 2) w += HINT_GLYPH_GAP + h->glyph_w[1];
    return w;
}

float hint_layout(HintItem *h, int n, float right) {
    float x = right;
    for (int i = n - 1; i >= 0; i--) {
        HintItem *it = &h[i];
        int ng = it->nglyphs < 1 ? 1 : it->nglyphs > 2 ? 2 : it->nglyphs;
        x -= it->label_w;
        it->label_x = x;
        x -= HINT_LABEL_GAP;
        for (int g = ng - 1; g >= 0; g--) {
            x -= it->glyph_w[g];
            it->glyph_cx[g] = x + it->glyph_w[g] / 2;
            if (g > 0) x -= HINT_GLYPH_GAP;
        }
        if (i > 0) x -= HINT_ITEM_GAP;
    }
    return x;
}
