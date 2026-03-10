; queries/highlights.scm — Trixie treesitter highlight queries

; ── Comments ──────────────────────────────────────────────────────────────────
(line_comment)  @comment
(hash_comment)  @comment

; ── Block structure ───────────────────────────────────────────────────────────
(block_name)  @keyword.type
(block_label) @label
"{" @punctuation.bracket
"}" @punctuation.bracket

; ── Directives ────────────────────────────────────────────────────────────────
(directive_name) @keyword.import

; ── Variable definition LHS ───────────────────────────────────────────────────
(variable_def
  (variable) @variable.builtin)

; ── Variable use ──────────────────────────────────────────────────────────────
(variable) @variable

; ── Assignment key ────────────────────────────────────────────────────────────
(assignment
  key: (key) @property)

; ── Values ────────────────────────────────────────────────────────────────────
(color)        @string.special
(boolean)      @boolean
(number)       @number
(string_value) @string

; ── Bind values ───────────────────────────────────────────────────────────────
(modifier) @keyword.modifier
(action)   @function.builtin
(key_sym)  @character
