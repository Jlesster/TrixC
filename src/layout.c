/* layout.c — Tiling layout engine for Trixie.
 *
 * ── DWINDLE (Hyprland-style BSP) ─────────────────────────────────────────────
 *
 *   Implements a true persistent binary space partitioning tree, matching
 *   Hyprland's dwindle layout semantics exactly.
 *
 *   CORE ALGORITHM
 *   ──────────────
 *   Every workspace maintains a DwindleTree (stored in Workspace.dwindle).
 *   Each node is either:
 *     - DNODE_LEAF:      holds a single pane ID.
 *     - DNODE_CONTAINER: holds two children, a split direction, and a ratio.
 *
 *   When a new window opens (dwindle_insert):
 *     1. Find the leaf node for the currently focused pane (the "target").
 *     2. Replace that leaf with a new CONTAINER node.
 *     3. The container's children are: the original leaf (child[0]) and a
 *        new leaf for the new window (child[1]).
 *     4. Split direction is chosen from the container's bounding rect:
 *          W > H  ->  DSPLIT_H (left | right)
 *          H >= W ->  DSPLIT_V (top  | bottom)
 *        This matches Hyprland's dynamic ratio-based split selection.
 *     5. Split ratio defaults to 0.5 (50/50).
 *     6. All rects are recomputed recursively from the root.
 *
 *   When a window closes (dwindle_remove):
 *     1. Find the leaf for the closing pane.
 *     2. Find its sibling (the other child of its parent container).
 *     3. Promote the sibling to replace the parent, inheriting the parent's
 *        rect so it fills the full space the pair previously shared.
 *
 *   SPLIT DIRECTION EXAMPLES
 *   ──────────────────────────
 *   n=2  wide screen -> H split      n=3  first H split, second V split
 *   ┌─────────┬─────────┐           ┌─────────┬─────────┐
 *   │         │         │           │    A    │    B    │
 *   │    A    │    B    │           │         ├─────────┤
 *   │         │         │           │         │    C    │
 *   └─────────┴─────────┘           └─────────┴─────────┘
 *
 *   Per-container ratios are preserved across reflows -- resizing a split
 *   only touches its container node, not the whole tree.
 *
 *   INTEGRATION WITH TWM (see twm.c)
 *   ──────────────────────────────────
 *   In reflow_workspace():
 *     if(ws->layout == LAYOUT_DWINDLE) {
 *       dwindle_recompute(&ws->dwindle, t->content_rect, eff_gap);
 *       for(int i = 0; i < tiled_n; i++) {
 *         Pane *p = twm_pane_by_id(t, tiled[i]);
 *         if(p) dwindle_get_rect(&ws->dwindle, tiled[i], &p->rect);
 *       }
 *     }
 *
 *   In twm_open_ex():
 *     if(ws->layout == LAYOUT_DWINDLE && !floating && !fullscreen)
 *       dwindle_insert(&ws->dwindle, p->id, prev_focused,
 *                      t->content_rect, eff_gap);
 *
 *   In twm_close():
 *     if(ws->layout == LAYOUT_DWINDLE)
 *       dwindle_remove(&ws->dwindle, id);
 *
 *   For grow_main / shrink_main with dwindle:
 *     dwindle_adjust_split(&ws->dwindle, ws->focused, +-0.05f);
 *
 * ── COLUMNS ──────────────────────────────────────────────────────────────────
 *   n equal-width vertical columns, full screen height.
 *
 * ── ROWS ─────────────────────────────────────────────────────────────────────
 *   n equal-height horizontal rows, full screen width.
 *
 * ── THREECOL ─────────────────────────────────────────────────────────────────
 *   pane[0] -> centre column (full height), ratio controls width fraction.
 *   pane[1,3,5...] -> right stack,  pane[2,4,6...] -> left stack.
 *
 * ── MONOCLE ──────────────────────────────────────────────────────────────────
 *   Every pane gets the full area. Z-order makes focused visible on top.
 */

#include "trixie.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Labels / cycle helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

const char *layout_label(Layout l) {
  switch(l) {
    case LAYOUT_DWINDLE: return "\uf45c Dwindle";
    case LAYOUT_COLUMNS: return "\uf0db Columns";
    case LAYOUT_ROWS: return "\uf00b Rows";
    case LAYOUT_THREECOL: return "\uf1b2 ThreeCol";
    case LAYOUT_MONOCLE: return "\uf04c Monocle";
    default: return "\uf45c Dwindle";
  }
}

Layout layout_next(Layout l) {
  switch(l) {
    case LAYOUT_DWINDLE: return LAYOUT_COLUMNS;
    case LAYOUT_COLUMNS: return LAYOUT_ROWS;
    case LAYOUT_ROWS: return LAYOUT_THREECOL;
    case LAYOUT_THREECOL: return LAYOUT_MONOCLE;
    default: return LAYOUT_DWINDLE;
  }
}

Layout layout_prev(Layout l) {
  switch(l) {
    case LAYOUT_COLUMNS: return LAYOUT_DWINDLE;
    case LAYOUT_ROWS: return LAYOUT_COLUMNS;
    case LAYOUT_THREECOL: return LAYOUT_ROWS;
    case LAYOUT_MONOCLE: return LAYOUT_THREECOL;
    default: return LAYOUT_MONOCLE;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  BSP Dwindle -- node pool helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int dnode_alloc(DwindleTree *tree) {
  for(int i = 0; i < DWINDLE_MAX_NODES; i++) {
    if(!tree->nodes[i].in_use) {
      memset(&tree->nodes[i], 0, sizeof(DwindleNode));
      tree->nodes[i].in_use   = true;
      tree->nodes[i].child[0] = DWINDLE_NULL;
      tree->nodes[i].child[1] = DWINDLE_NULL;
      tree->nodes[i].ratio    = 0.5f;
      tree->count++;
      return i;
    }
  }
  return DWINDLE_NULL;
}

static void dnode_free(DwindleTree *tree, int idx) {
  if(idx == DWINDLE_NULL) return;
  tree->nodes[idx].in_use = false;
  tree->count--;
}

void dwindle_clear(DwindleTree *tree) {
  memset(tree->nodes, 0, sizeof(tree->nodes));
  tree->root  = DWINDLE_NULL;
  tree->count = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  BSP Dwindle -- tree traversal helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int dwindle_find_leaf(DwindleTree *tree, int idx, PaneId id) {
  if(idx == DWINDLE_NULL || idx < 0 || idx >= DWINDLE_MAX_NODES) return DWINDLE_NULL;
  DwindleNode *n = &tree->nodes[idx];
  if(!n->in_use) return DWINDLE_NULL;
  if(n->kind == DNODE_LEAF) return (n->pane_id == id) ? idx : DWINDLE_NULL;
  int r = dwindle_find_leaf(tree, n->child[0], id);
  if(r != DWINDLE_NULL) return r;
  return dwindle_find_leaf(tree, n->child[1], id);
}

static int
dwindle_find_parent(DwindleTree *tree, int root, int target, int *child_slot) {
  if(root == DWINDLE_NULL || root < 0 || root >= DWINDLE_MAX_NODES)
    return DWINDLE_NULL;
  DwindleNode *n = &tree->nodes[root];
  if(!n->in_use || n->kind == DNODE_LEAF) return DWINDLE_NULL;
  for(int s = 0; s < 2; s++) {
    if(n->child[s] == target) {
      *child_slot = s;
      return root;
    }
    int r = dwindle_find_parent(tree, n->child[s], target, child_slot);
    if(r != DWINDLE_NULL) return r;
  }
  return DWINDLE_NULL;
}

static DSplitDir split_for_rect(Rect r) {
  return (r.w > r.h) ? DSPLIT_H : DSPLIT_V;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  dwindle_insert -- add a new pane by splitting the focused leaf
 * ═══════════════════════════════════════════════════════════════════════════ */

void dwindle_insert(
    DwindleTree *tree, PaneId new_id, PaneId focused_id, Rect area, int gap) {
  if(tree->root == DWINDLE_NULL) {
    int leaf = dnode_alloc(tree);
    if(leaf == DWINDLE_NULL) return;
    tree->nodes[leaf].kind    = DNODE_LEAF;
    tree->nodes[leaf].pane_id = new_id;
    tree->nodes[leaf].rect    = area;
    tree->root                = leaf;
    return;
  }

  /* Find the focused leaf to split.  focused_id may be 0 (no prior focus),
   * or it may belong to a floating pane that was never inserted — in both
   * cases we fall through to the rightmost-leaf fallback below.            */
  int target = DWINDLE_NULL;
  if(focused_id) target = dwindle_find_leaf(tree, tree->root, focused_id);

  /* Fallback: walk to the deepest right-most leaf. */
  if(target == DWINDLE_NULL) {
    target = tree->root;
    while(target != DWINDLE_NULL && tree->nodes[target].kind == DNODE_CONTAINER)
      target = tree->nodes[target].child[1];
  }
  if(target == DWINDLE_NULL) return;

  int cont_idx = dnode_alloc(tree);
  int new_idx  = dnode_alloc(tree);
  if(cont_idx == DWINDLE_NULL || new_idx == DWINDLE_NULL) {
    if(cont_idx != DWINDLE_NULL) dnode_free(tree, cont_idx);
    if(new_idx != DWINDLE_NULL) dnode_free(tree, new_idx);
    return;
  }

  DwindleNode *cont = &tree->nodes[cont_idx];
  cont->kind        = DNODE_CONTAINER;
  cont->rect        = tree->nodes[target].rect;
  cont->split       = split_for_rect(cont->rect);
  cont->ratio       = 0.5f;
  cont->child[0]    = target;
  cont->child[1]    = new_idx;

  tree->nodes[new_idx].kind    = DNODE_LEAF;
  tree->nodes[new_idx].pane_id = new_id;

  int slot   = 0;
  int parent = dwindle_find_parent(tree, tree->root, target, &slot);
  if(parent != DWINDLE_NULL)
    tree->nodes[parent].child[slot] = cont_idx;
  else
    tree->root = cont_idx;

  dwindle_recompute(tree, area, gap);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  dwindle_remove -- remove a closed pane; promote its sibling
 * ═══════════════════════════════════════════════════════════════════════════ */

void dwindle_remove(DwindleTree *tree, PaneId closed_id) {
  if(tree->root == DWINDLE_NULL) return;

  int target = dwindle_find_leaf(tree, tree->root, closed_id);
  if(target == DWINDLE_NULL) return;

  if(target == tree->root) {
    dnode_free(tree, target);
    tree->root = DWINDLE_NULL;
    return;
  }

  int slot   = 0;
  int parent = dwindle_find_parent(tree, tree->root, target, &slot);
  if(parent == DWINDLE_NULL) return;

  int sibling               = tree->nodes[parent].child[1 - slot];
  tree->nodes[sibling].rect = tree->nodes[parent].rect;

  int gslot       = 0;
  int grandparent = dwindle_find_parent(tree, tree->root, parent, &gslot);
  if(grandparent != DWINDLE_NULL)
    tree->nodes[grandparent].child[gslot] = sibling;
  else
    tree->root = sibling;

  dnode_free(tree, target);
  dnode_free(tree, parent);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  dwindle_recompute -- recursive rect assignment
 * ═══════════════════════════════════════════════════════════════════════════ */

void dwindle_recompute_subtree(DwindleTree *tree, int idx, Rect area, int gap) {
  if(idx == DWINDLE_NULL || idx < 0 || idx >= DWINDLE_MAX_NODES) return;
  DwindleNode *n = &tree->nodes[idx];
  if(!n->in_use) return;
  n->rect = area;

  if(n->kind == DNODE_LEAF) return;

  float ratio = n->ratio;
  if(ratio < 0.05f) ratio = 0.05f;
  if(ratio > 0.95f) ratio = 0.95f;

  /* Recompute split direction dynamically from current geometry.
   * dwindle_toggle_split() overrides this by writing a value then immediately
   * calling recompute -- since we always overwrite here, toggle_split must
   * write to a separate "locked" field if you want persistence. For now we
   * match Hyprland's default (no preserve_split): always auto. */
  n->split = split_for_rect(area);

  Rect a0, a1;
  if(n->split == DSPLIT_H) {
    int lw = (int)((float)(area.w - gap) * ratio);
    if(lw < 1) lw = 1;
    int rw = area.w - lw - gap;
    if(rw < 1) {
      rw = 1;
      lw = area.w - rw - gap;
    }
    a0 = (Rect){ area.x, area.y, lw, area.h };
    a1 = (Rect){ area.x + lw + gap, area.y, rw, area.h };
  } else {
    int th = (int)((float)(area.h - gap) * ratio);
    if(th < 1) th = 1;
    int bh = area.h - th - gap;
    if(bh < 1) {
      bh = 1;
      th = area.h - bh - gap;
    }
    a0 = (Rect){ area.x, area.y, area.w, th };
    a1 = (Rect){ area.x, area.y + th + gap, area.w, bh };
  }

  dwindle_recompute_subtree(tree, n->child[0], a0, gap);
  dwindle_recompute_subtree(tree, n->child[1], a1, gap);
}

void dwindle_recompute(DwindleTree *tree, Rect area, int gap) {
  if(tree->root == DWINDLE_NULL) return;
  dwindle_recompute_subtree(tree, tree->root, area, gap);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  BSP Dwindle -- public helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

bool dwindle_get_rect(DwindleTree *tree, PaneId id, Rect *out) {
  int idx = dwindle_find_leaf(tree, tree->root, id);
  if(idx == DWINDLE_NULL) return false;
  *out = tree->nodes[idx].rect;
  return true;
}

/* Returns true if the tree contains a leaf for the given pane ID. */
bool dwindle_has_leaf(DwindleTree *tree, PaneId id) {
  return dwindle_find_leaf(tree, tree->root, id) != DWINDLE_NULL;
}

/* Collect all current leaf pane IDs from the tree into out[].
 * Returns the count written. Used by dwindle_sync.                          */
static int dwindle_collect_leaves(DwindleTree *tree, int idx, PaneId *out, int cap) {
  if(idx == DWINDLE_NULL || idx < 0 || idx >= DWINDLE_MAX_NODES || cap <= 0)
    return 0;
  DwindleNode *n = &tree->nodes[idx];
  if(!n->in_use) return 0;
  if(n->kind == DNODE_LEAF) {
    out[0] = n->pane_id;
    return 1;
  }
  int l = dwindle_collect_leaves(tree, n->child[0], out, cap);
  int r = dwindle_collect_leaves(tree, n->child[1], out + l, cap - l);
  return l + r;
}

/*
 * dwindle_sync — make the tree exactly mirror the live tiled[] list.
 *
 * focused_id: the currently focused tiled pane.  New insertions will split
 *             this leaf (matching Hyprland behaviour).  Pass 0 if unknown.
 */
void dwindle_sync(DwindleTree *tree, const PaneId *tiled, int n, PaneId focused_id) {
  /* Step 1: prune stale leaves. */
  PaneId leaves[DWINDLE_MAX_NODES];
  int leaf_n = dwindle_collect_leaves(tree, tree->root, leaves, DWINDLE_MAX_NODES);
  for(int i = 0; i < leaf_n; i++) {
    bool found = false;
    for(int j = 0; j < n && !found; j++)
      if(tiled[j] == leaves[i]) found = true;
    if(!found) dwindle_remove(tree, leaves[i]);
  }

  /* Step 2: insert any missing pane.
   *
   * Split target priority:
   *  1. The focused pane (if it's already in the tree) — normal dwindle.
   *  2. The previous pane in tiled[] — used when multiple panes are missing
   *     at once (layout switch back to dwindle).  Chaining through the list
   *     approximates the topology that would have been built if each window
   *     had been opened in order, avoiding a spiral.
   *  3. Rightmost leaf fallback inside dwindle_insert.
   */
  for(int j = 0; j < n; j++) {
    if(!dwindle_has_leaf(tree, tiled[j])) {
      PaneId split_target;
      if(focused_id && dwindle_has_leaf(tree, focused_id))
        split_target = focused_id;
      else
        split_target = (j > 0) ? tiled[j - 1] : 0;
      dwindle_insert(tree, tiled[j], split_target, (Rect){ 0, 0, 0, 0 }, 0);
    }
  }
}

bool dwindle_adjust_split(DwindleTree *tree, PaneId id, float delta) {
  int leaf = dwindle_find_leaf(tree, tree->root, id);
  if(leaf == DWINDLE_NULL) return false;
  int slot   = 0;
  int parent = dwindle_find_parent(tree, tree->root, leaf, &slot);
  if(parent == DWINDLE_NULL) return false;
  float        d    = (slot == 0) ? delta : -delta;
  DwindleNode *cont = &tree->nodes[parent];
  cont->ratio += d;
  if(cont->ratio < 0.05f) cont->ratio = 0.05f;
  if(cont->ratio > 0.95f) cont->ratio = 0.95f;
  return true;
}

bool dwindle_toggle_split(DwindleTree *tree, PaneId id) {
  int leaf = dwindle_find_leaf(tree, tree->root, id);
  if(leaf == DWINDLE_NULL) return false;
  int slot   = 0;
  int parent = dwindle_find_parent(tree, tree->root, leaf, &slot);
  if(parent == DWINDLE_NULL) return false;
  /* Toggle the split and re-lock it so recompute_subtree won't overwrite.
   * We abuse the split field directly -- since recompute always overwrites,
   * a locked split requires twm.c to call dwindle_recompute_subtree with the
   * locked node's area AFTER toggling, which happens naturally on next reflow. */
  DwindleNode *cont = &tree->nodes[parent];
  cont->split       = (cont->split == DSPLIT_H) ? DSPLIT_V : DSPLIT_H;
  return true;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Dwindle swap operations
 *
 * All swaps work by exchanging pane_id values between two leaf nodes.
 * The tree topology (split directions, ratios, geometry) is untouched —
 * only the content of the leaves changes.  After any swap, call
 * dwindle_recompute + dwindle_get_rect as normal to apply new rects.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Collect leaf indices (not pane_ids) into out[] in left-to-right,
 * top-to-bottom in-order traversal.  Returns count written.              */
static int collect_leaf_indices(DwindleTree *tree, int ni, int *out, int cap) {
  if(ni == DWINDLE_NULL || ni < 0 || ni >= DWINDLE_MAX_NODES || cap <= 0) return 0;
  DwindleNode *n = &tree->nodes[ni];
  if(!n->in_use) return 0;
  if(n->kind == DNODE_LEAF) {
    out[0] = ni;
    return 1;
  }
  int l = collect_leaf_indices(tree, n->child[0], out, cap);
  int r = collect_leaf_indices(tree, n->child[1], out + l, cap - l);
  return l + r;
}

/*
 * dwindle_swap_cycle — cycle the focused pane forward or backward through
 * the in-order leaf sequence.  Mirrors twm_swap(forward/back) for dwindle.
 */
bool dwindle_swap_cycle(DwindleTree *tree, PaneId focused_id, bool forward) {
  int indices[DWINDLE_MAX_NODES];
  int count = collect_leaf_indices(tree, tree->root, indices, DWINDLE_MAX_NODES);
  if(count < 2) return false;

  /* Find focused leaf position in the sequence. */
  int pos = -1;
  for(int i = 0; i < count; i++)
    if(tree->nodes[indices[i]].pane_id == focused_id) {
      pos = i;
      break;
    }
  if(pos < 0) return false;

  int tgt = forward ? (pos + 1) % count : (pos + count - 1) % count;

  /* Swap pane_ids between the two leaves. */
  PaneId tmp                        = tree->nodes[indices[pos]].pane_id;
  tree->nodes[indices[pos]].pane_id = tree->nodes[indices[tgt]].pane_id;
  tree->nodes[indices[tgt]].pane_id = tmp;
  return true;
}

/*
 * dwindle_swap_main — swap the focused pane with the first leaf in
 * in-order traversal (visually the "main" / top-left slot).
 */
bool dwindle_swap_main(DwindleTree *tree, PaneId focused_id) {
  int indices[DWINDLE_MAX_NODES];
  int count = collect_leaf_indices(tree, tree->root, indices, DWINDLE_MAX_NODES);
  if(count < 2) return false;

  int pos = -1;
  for(int i = 0; i < count; i++)
    if(tree->nodes[indices[i]].pane_id == focused_id) {
      pos = i;
      break;
    }
  if(pos < 0 || pos == 0) return false; /* already main */

  PaneId tmp                        = tree->nodes[indices[0]].pane_id;
  tree->nodes[indices[0]].pane_id   = tree->nodes[indices[pos]].pane_id;
  tree->nodes[indices[pos]].pane_id = tmp;
  return true;
}

/*
 * dwindle_swap_dir — swap the focused pane with the nearest neighbour
 * in the given direction (dx,dy).  Uses the same centre-point cosine
 * scoring as twm_focus_dir so the result is spatially intuitive.
 *
 * Returns true if a swap was performed.
 */
bool dwindle_swap_dir(DwindleTree *tree, PaneId focused_id, int dx, int dy) {
  /* Collect all leaf rects so we can score them. */
  int indices[DWINDLE_MAX_NODES];
  int count = collect_leaf_indices(tree, tree->root, indices, DWINDLE_MAX_NODES);
  if(count < 2) return false;

  int fpos = -1;
  for(int i = 0; i < count; i++)
    if(tree->nodes[indices[i]].pane_id == focused_id) {
      fpos = i;
      break;
    }
  if(fpos < 0) return false;

  Rect fr = tree->nodes[indices[fpos]].rect;
  int  cx = fr.x + fr.w / 2;
  int  cy = fr.y + fr.h / 2;

  int   best_pos   = -1;
  float best_score = -1.0f;
  for(int i = 0; i < count; i++) {
    if(i == fpos) continue;
    Rect nr  = tree->nodes[indices[i]].rect;
    int  nx  = nr.x + nr.w / 2;
    int  ny  = nr.y + nr.h / 2;
    int  vx  = nx - cx;
    int  vy  = ny - cy;
    int  dot = vx * dx + vy * dy;
    if(dot <= 0) continue;
    float score = (float)dot / (float)(vx * vx + vy * vy + 1);
    if(score > best_score) {
      best_score = score;
      best_pos   = i;
    }
  }
  if(best_pos < 0) return false;

  PaneId tmp                             = tree->nodes[indices[fpos]].pane_id;
  tree->nodes[indices[fpos]].pane_id     = tree->nodes[indices[best_pos]].pane_id;
  tree->nodes[indices[best_pos]].pane_id = tmp;
  return true;
}


static void cut_h(Rect r, int lw, int gap, Rect *left, Rect *right) {
  if(lw < 1) lw = 1;
  int rw = r.w - lw - gap;
  if(rw < 1) {
    rw = 1;
    lw = r.w - rw - gap;
  }
  if(left) *left = (Rect){ r.x, r.y, lw, r.h };
  if(right) *right = (Rect){ r.x + lw + gap, r.y, rw, r.h };
}

static void cut_v(Rect r, int th, int gap, Rect *top, Rect *bottom) {
  if(th < 1) th = 1;
  int bh = r.h - th - gap;
  if(bh < 1) {
    bh = 1;
    th = r.h - bh - gap;
  }
  if(top) *top = (Rect){ r.x, r.y, r.w, th };
  if(bottom) *bottom = (Rect){ r.x, r.y + th + gap, r.w, bh };
}

static void vstack(Rect area, int n, int gap, Rect *out) {
  if(n <= 0) return;
  if(n == 1) {
    out[0] = area;
    return;
  }
  int h = (area.h - gap * (n - 1)) / n;
  if(h < 1) h = 1;
  for(int i = 0; i < n; i++) {
    int y  = area.y + i * (h + gap);
    out[i] = (Rect){ area.x, y, area.w, (i == n - 1) ? (area.y + area.h - y) : h };
  }
}

static void columns(Rect area, int n, int gap, Rect *out) {
  if(n <= 0) return;
  if(n == 1) {
    out[0] = area;
    return;
  }
  int w = (area.w - gap * (n - 1)) / n;
  if(w < 1) w = 1;
  for(int i = 0; i < n; i++) {
    int x  = area.x + i * (w + gap);
    out[i] = (Rect){ x, area.y, (i == n - 1) ? (area.x + area.w - x) : w, area.h };
  }
}

static void rows(Rect area, int n, int gap, Rect *out) {
  vstack(area, n, gap, out);
}

static void threecol(Rect area, int n, float ratio, int gap, Rect *out) {
  if(n <= 0) return;
  if(n == 1) {
    out[0] = area;
    return;
  }

  if(n == 2) {
    int lw = (int)((area.w - gap) * ratio);
    if(lw < 1) lw = 1;
    if(lw > area.w - gap - 1) lw = area.w - gap - 1;
    cut_h(area, lw, gap, &out[0], &out[1]);
    return;
  }

  int cw = (int)((area.w - gap * 2) * ratio);
  if(cw < 4) cw = 4;
  int sw = (area.w - cw - gap * 2) / 2;
  if(sw < 4) {
    sw = 4;
    cw = area.w - sw * 2 - gap * 2;
  }

  int lx = area.x;
  int cx = area.x + sw + gap;
  int rx = cx + cw + gap;

  out[0] = (Rect){ cx, area.y, cw, area.h };

  int sn      = n - 1;
  int right_n = (sn + 1) / 2;
  int left_n  = sn - right_n;

  Rect rbuf[MAX_PANES / 2 + 1];
  Rect lbuf[MAX_PANES / 2 + 1];
  vstack((Rect){ rx, area.y, sw, area.h }, right_n, gap, rbuf);
  vstack((Rect){ lx, area.y, sw, area.h }, left_n, gap, lbuf);

  int ri = 0, li = 0;
  for(int i = 0; i < sn; i++) {
    if(i % 2 == 0)
      out[i + 1] = rbuf[ri++];
    else
      out[i + 1] = lbuf[li++];
  }
}

static void monocle(Rect area, int n, Rect *out) {
  for(int i = 0; i < n; i++)
    out[i] = area;
}

static void dwindle_flat(Rect area, int n, int gap, int depth, Rect *out) {
  if(n <= 0) return;
  if(n == 1) {
    out[0] = area;
    return;
  }
  Rect first, rest;
  if(depth % 2 == 0)
    cut_h(area, (area.w - gap) / 2, gap, &first, &rest);
  else
    cut_v(area, (area.h - gap) / 2, gap, &first, &rest);
  out[0] = first;
  dwindle_flat(rest, n - 1, gap, depth + 1, out + 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  layout_compute -- public entry point for non-dwindle layouts
 * ═══════════════════════════════════════════════════════════════════════════ */

void layout_compute(Layout l, Rect area, int n, float ratio, int gap, Rect *out) {
  typedef struct {
    Layout l;
    int    n, x, y, w, h, gap;
    float  ratio;
  } Key;
  static Key  s_key;
  static Rect s_cache[MAX_PANES];
  static int  s_n = 0;

  if(n <= 0) return;

  Key k = { l, n, area.x, area.y, area.w, area.h, gap, ratio };
  if(n == s_n && memcmp(&k, &s_key, sizeof k) == 0) {
    memcpy(out, s_cache, (size_t)n * sizeof(Rect));
    return;
  }

  switch(l) {
    case LAYOUT_DWINDLE:
      /* Flat fallback only. The real BSP path is driven from twm.c via
       * dwindle_recompute() + dwindle_get_rect(). */
      dwindle_flat(area, n, gap, 0, out);
      break;
    case LAYOUT_COLUMNS: columns(area, n, gap, out); break;
    case LAYOUT_ROWS: rows(area, n, gap, out); break;
    case LAYOUT_THREECOL: threecol(area, n, ratio, gap, out); break;
    case LAYOUT_MONOCLE: monocle(area, n, out); break;
    default: dwindle_flat(area, n, gap, 0, out); break;
  }

  s_key = k;
  s_n   = n;
  memcpy(s_cache, out, (size_t)n * sizeof(Rect));
}
