module.exports = grammar({
  name: "trixie",

  extras: $ => [/\s/, $.line_comment],

  conflicts: $ => [
    [$.bind_value, $._value],
  ],

  rules: {
    source_file: $ => repeat($._statement),

    _statement: $ => choice(
      $.variable_def,
      $.directive,
      $.block,
      $.assignment,
      $.hash_comment,
    ),

    // // line comment  (in extras so it can appear anywhere)
    line_comment: $ => /\/\/[^\n]*/,

    // # comment — explicit rule so color literals in values take priority
    // Only valid at start of a line (after optional whitespace)
    hash_comment: $ => /#[^\n]*/,

    // $name = value
    variable_def: $ => seq(
      $.variable,
      "=",
      $._value,
    ),

    // source = ..., exec = ..., exec-once = ...
    directive: $ => seq(
      field("name", alias(
        choice("source", "exec", "exec-once"),
        $.directive_name
      )),
      "=",
      field("value", $.string_value),
    ),

    // block { ... }  or  block label { ... }
    block: $ => seq(
      field("type", $.block_name),
      optional(field("label", $.block_label)),
      "{",
      repeat($._block_statement),
      "}",
    ),

    block_name: $ => choice(
      "general", "decoration", "colors", "colour",
      "input", "keyboard", "bar", "animations", "overlay",
      "monitor", "scratchpad", "bar_module", "dev_lang", "workspace",
    ),

    block_label: $ => /[^\s{]+/,

    _block_statement: $ => choice(
      $.variable_def,
      $.assignment,
      $.hash_comment,
    ),

    // key = value
    assignment: $ => seq(
      field("key", $.key),
      "=",
      field("value", $._value),
    ),

    key: $ => /[a-zA-Z_][a-zA-Z0-9_-]*/,

    _value: $ => choice(
      $.color,
      $.boolean,
      $.number,
      $.variable,
      $.bind_value,
      $.string_value,
    ),

    // bind = SUPER, q, close  etc.
    // We match this as a comma-separated sequence starting with a modifier.
    bind_value: $ => seq(
      $.modifier,
      repeat(seq(optional(","), $.modifier)),
      ",",
      $.key_sym,
      ",",
      $.action,
      optional(seq(",", $.string_value)),
    ),

    modifier: $ => choice(
      "SUPER", "SHIFT", "CTRL", "ALT", "META",
      "HYPER", "MOD1", "MOD2", "MOD3", "MOD4", "MOD5",
    ),

    key_sym: $ => /[^\s,\n]+/,

    action: $ => choice(
      "exec", "close", "fullscreen",
      "togglefloating", "toggle_float",
      "togglebar", "toggle_bar",
      "movefocus", "focus",
      "movewindow", "move",
      "workspace", "movetoworkspace", "move_to_workspace",
      "nextlayout", "next_layout", "prevlayout", "prev_layout",
      "growmain", "grow_main", "shrinkmain", "shrink_main",
      "nextworkspace", "next_workspace", "prevworkspace", "prev_workspace",
      "swapwithmaster", "swap_main",
      "switchvt", "switch_vt",
      "scratchpad", "reload", "exit", "quit",
      "emergency_quit", "emergencyquit", "resize_ratio",
    ),

    // Color literals — listed before string_value so they win on conflict.
    // 0xRRGGBBAA, #RRGGBBAA, #RRGGBB, rgba(...), rgb(...)
    color: $ => choice(
      /0[xX][0-9A-Fa-f]{6,8}/,
      /#[0-9A-Fa-f]{8}/,
      /#[0-9A-Fa-f]{6}/,
      /rgba?\([0-9A-Fa-f]{6,8}\)/,
    ),

    boolean: $ => choice(
      "true", "false", "yes", "no", "on", "off",
    ),

    // number with optional unit suffix, or bare float/int
    number: $ => token(choice(
      /\d+(\.\d+)?(px|hz|Hz|ms|%)/,
      /\d+\.\d+/,
      /-?\d+/,
    )),

    variable: $ => /\$[A-Za-z_][A-Za-z0-9_]*/,

    // fallback: everything else on the line (path, font name, seat0, etc.)
    string_value: $ => /[^\s\n#][^\n]*/,
  },
});
