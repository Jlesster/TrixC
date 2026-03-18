#!/usr/bin/env bash
# Run from project root: bash fix_last.sh
set -e

# ── Fix 1: shader needs GLES3 for VAO + framebuffer blit ─────────────────
# glGenVertexArrays, glBlitFramebuffer, GL_READ_FRAMEBUFFER are in GLES 3.0
# libglvnd's gl2.h only covers GLES 2.0; gl3.h adds the GLES 3.0 API.
for f in include/shader.h src/shader.c; do
  if ! grep -q 'GLES3/gl3.h' "$f"; then
    sed -i 's|#include <GLES2/gl2.h>|#include <GLES2/gl2.h>\n#include <GLES3/gl3.h>|' "$f"
    echo "✓ $f: added GLES3/gl3.h"
  else
    echo "  $f: GLES3/gl3.h already present"
  fi
done

# ── Fix 2: wlr_scene_surface_create returns wlr_scene_surface*, not tree ──
# In wlroots 0.18 the xwayland scene helper was renamed/restructured.
# wlr_scene_surface_create(parent_tree, wlr_surface*) → wlr_scene_surface*
# The containing tree is the parent we passed, but we need scene_tree to be
# a wlr_scene_tree* for reparenting (wlr_scene_node_reparent etc).
#
# Solution: create an explicit sub-tree for xwayland views, then attach
# the surface inside it. scene_tree = the sub-tree.
python3 - <<'EOF'
import re

with open('src/main.c', 'r') as f:
    src = f.read()

# Pattern 1 (override_redirect path, layer_floating):
#   v->scene_tree = wlr_scene_surface_create(s->layer_floating, xs->surface);
# Replace with: create a tree, then attach surface inside it
old1 = 'v->scene_tree = wlr_scene_surface_create(s->layer_floating, xs->surface);'
new1 = ('v->scene_tree = wlr_scene_tree_create(s->layer_floating);\n'
        '    wlr_scene_surface_create(v->scene_tree, xs->surface);')

# Pattern 2 (normal xwayland path, layer_windows):
#   v->scene_tree = wlr_scene_surface_create(s->layer_windows, xs->surface);
old2 = 'v->scene_tree = wlr_scene_surface_create(s->layer_windows, xs->surface);'
new2 = ('v->scene_tree = wlr_scene_tree_create(s->layer_windows);\n'
        '  wlr_scene_surface_create(v->scene_tree, xs->surface);')

if old1 in src:
    src = src.replace(old1, new1)
    print('✓ Fixed override_redirect xwayland scene_tree')
else:
    print('WARNING: pattern 1 not found - may already be fixed or different whitespace')

if old2 in src:
    src = src.replace(old2, new2)
    print('✓ Fixed normal xwayland scene_tree')
else:
    print('WARNING: pattern 2 not found - may already be fixed or different whitespace')

with open('src/main.c', 'w') as f:
    f.write(src)
EOF

echo ""
echo "Patches applied. Rebuilding..."
rm -rf build && meson setup build && ninja -C build 2>&1 | grep -E "error:|FAILED|^ninja:" | grep -v "^ninja: Entering" | head -30
