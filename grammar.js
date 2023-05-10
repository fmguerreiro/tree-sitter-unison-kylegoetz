const { sep } = require('./grammar/util')
const misc = require('./grammar/misc')
const comments = require('./grammar/comments')
const expressions = require('./grammar/expression')
const blocks = require('./grammar/blocks')
const stmt = require('./grammar/statement')

module.exports = grammar({
  name: 'unison',
  precedences: $ => [
    ['_expression'],
    ['literal_function'],
  ],
  conflicts: $ => [
    // [$.block, $.statement],
    // [$.block, $.term_definition],
    [$._expression, $.literal_function],
    // [$._expression, $.functional_expression],
    [$.functional_expression],
  ],
  externals: $ => [
    $._layout_semicolon,
    $._layout_start,
    $._layout_end,
    $._dot,
    $._where,
    $._varsym,
    $.comment,
    $.fold,
    $._comma,
    $._in,
    $._indent,
    $._empty,
  ],
  rules: {
    unison: $ => repeat(
      choice(
        $.term_declaration,
        // $.inline_comment,
        $.fold,
        $.comment_multiline,
        $.comment,
        // seq(/(\r)?\n/, $.fold),
      ),
    ),
    
    ...literals,
    // ...misc,
    ...comments,
    ...blocks,
    ...expressions,
    ...stmt,
    
    // fold: $ => $._fold,
    kw_forall: $ => choice("forall", "∀"),
    kw_equals: $ => '=',
    
    type_signature_colon: $ => ':',
    term_name: $ => $._regular_identifier,
    type_signature: $ => seq(
      $.term_name,
      $.type_signature_colon,
      $.type,
      // $._eol,
    ),
    
    type_name: $ => $._regular_identifier,
    
    name: $ => $._regular_identifier,
    // _term_rhs: $ => $._expression,
    
    term_definition: $ => seq(
      $.name,
      repeat($.type_variable),
      $.kw_equals,
      $._expression
    ),
    
    term_declaration: $ => seq(
      optional($.type_signature),
      $.term_definition
    ),
    
    _lowercase_regular_identifier: $ => /[a-z]+/,
    regular_identifier: $ => $._regular_identifier,
    _regular_identifier: $ => /[a-zA-Z_][a-zA-Z0-9_]*/,
    
    type_variable: $ => $._lowercase_regular_identifier,
    
    type_arrow: $ => '->',
    
    type: $ => seq(
      optional(seq(
        $.kw_forall, 
        repeat1($.type_variable), 
        token.immediate('.')
      )), 
      sep1($.type_arrow, $.type_name)
    ),
    
    kw_termlink: $ => 'termLink',
    kw_typelink: $ => 'typeLink',
    
    _reserved_kw: $ => choice(
      '=',
      ':',
      '->',
      '\'',
      'do',
      '|',
      '!',
      '`',
      'if',
      'then',
      'else',
      $.kw_forall,
      'handle',
      'unique',
      'structural',
      'where',
      'use',
      '&&',
      '||',
      'true',
      'false',
      'type',
      'ability',
      'alias',
      $.kw_let,
      'namespace',
      'cases',
      'match',
      'with',
      $.kw_termlink,
      $.kw_typelink,
    )
  },
})
