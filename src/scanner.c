/**
 * Print input and result information.
 */
#define DEBUG 1

#include "tree_sitter/parser.h"
#include <assert.h>
#ifdef DEBUG
#include <stdio.h>
#endif
#include <string.h>
#include <wctype.h>
#include <stdlib.h>


// Short circuit
#define SHORT_SCANNER if (res.finished) return res;
#define PEEK state->lexer->lookahead
// Move the parser position one character to the right.
#define S_ADVANCE state->lexer->advance(state->lexer, false)
#define S_SKIP state->lexer->advance(state->lexer, true)
#define SYM(s) (state->symbols[s])

#ifdef DEBUG
#define DEBUG_PRINTF(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
#define DEBUG_PRINTF(...) do{ } while ( false )
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define VEC_RESIZE(vec, _cap) \
  (vec)->data = realloc((vec)->data, (_cap) * sizeof((vec)->data[0])); \
  assert((vec)->data != NULL); \
  (vec)->cap = (_cap);

#define VEC_GROW(vec, _cap) if ((vec)->cap < (_cap)) { VEC_RESIZE((vec), (_cap)); }\

#define VEC_PUSH(vec, el) \
  if ((vec)->cap == (vec)->len) { VEC_RESIZE((vec), MAX(20, (vec)->len * 2)); } \
  (vec)->data[(vec)->len++] = (el);

#define VEC_POP(vec) (vec)->len--;

#define VEC_BACK(vec) ((vec)->data[(vec)->len - 1])

#define VEC_FREE(vec) { if ((vec)->data != NULL) free((vec)->data); }

// ---------
// Symbols
// ---------

typedef enum {
    SEMICOLON,
    START,
    END,
    DOT,
    WHERE,
    VARSYM,
    COMMENT,
    FOLD,
    COMMA,
    IN,
    INDENT,
    EMPTY,
    FAIL, // always last in list
} Sym;

#ifdef DEBUG
static char *sym_names[] = {
    "semicolon",
    "start",
    "end",
    "dot",
    "where",
    "varsym",
    "comment",
    "fold",
    "comma",
    "in",
    "indent",
    "empty",
    "fail",
};
#endif

/**
 * The parser appears to call `scan` with all symbols declared as valid directly after it encountered an error, so
 * this function is used to detect them.
 */
static bool all_syms(const bool *syms) {
  for (int i = 0; i <= EMPTY; i++) {
    if (!syms[i]) return false;
  }
  return true;
}

#ifdef DEBUG
/**
 * Produce a comma-separated string of valid symbols.
 */
static void debug_valid(const bool *syms) {
  if (all_syms(syms)) {
    DEBUG_PRINTF("all");
    return;
  }
  bool fst = true;
  DEBUG_PRINTF("\"");
  for (Sym i = SEMICOLON; i <= EMPTY; i++) {
    if (syms[i]) {
      if (!fst) DEBUG_PRINTF(",");
      DEBUG_PRINTF("%s", sym_names[i]);
      fst = false;
    }
  }
  DEBUG_PRINTF("\"");
}
#endif

// State
typedef struct {
    uint32_t len;
    uint32_t cap;
    uint16_t *data;
} indent_vec;

// --------------------------------------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------------------------------------

/**
 * This structure contains the external and internal state.
 *
 * The parser provides the lexer interface and the list of valid symbols.
 *
 * The internal state consists of a stack of indentation widths that is manipulated whenever a layout is started or
 * terminated.
 */
typedef struct {
    TSLexer *lexer;
    const bool *symbols;
    indent_vec *indents;
#ifdef DEBUG
    int marked;
    char *marked_by;
    bool needs_free;
#endif
} State;

State state_new(TSLexer *l, const bool * restrict vs, indent_vec *is) {
  return (State) {
    .lexer = l,
    .symbols = vs,
    .indents = is,
#ifdef DEBUG
    .marked = -1,
    .marked_by = "",
    .needs_free = false,
#endif
  };
}

#ifdef DEBUG
static void debug_indents(indent_vec *indents) {
  if (indents->len == 0) DEBUG_PRINTF("empty");
  bool empty = true;
  for (size_t i = 0; i < indents->len; i++) {
    if (!empty) DEBUG_PRINTF("-");
    DEBUG_PRINTF("%d", indents->data[i]);
    empty = false;
  }
}

void debug_state(State *state) {
  DEBUG_PRINTF("State { syms = ");
  debug_valid(state->symbols);
  DEBUG_PRINTF("col = %d", state->lexer->get_column(state->lexer));
  DEBUG_PRINTF(", indents = ");
  debug_indents(state->indents);
  DEBUG_PRINTF(" }\n");
}
#endif

/**
 * These functions provide the basic interface to the lexer.
 * They are not defined as members for easier composition.
 */
static bool is_eof(State *state) { return state->lexer->eof(state->lexer); }

/**
 * The parser's position in the current line.
 */
static uint32_t column(State *state) {
  return is_eof(state) ? 0 : state->lexer->get_column(state->lexer);
}

/**
 * Instruct the lexer that the current position is the end of the potentially detected symbol, causing the next run to
 * be started after this character in the success case.
 *
 * This is useful if the validity of the detected symbol depends on what follows, e.g. in the case of a layout end
 * before a `where` token.
 */

// Only use string literals we actually need
#ifdef DEBUG
static void MARK(char *marked_by, bool needs_free, State *state) {
  state->marked = column(state);
  if (state->needs_free) free(state->marked_by);
  state->marked_by = marked_by;
  state->needs_free = needs_free;
  state->lexer->mark_end(state->lexer);
}
#else
#define MARK(s, nf, state) state->lexer->mark_end(state->lexer);
#endif

// --------------------------------------------------------------------------------------------------------
// Condition
// --------------------------------------------------------------------------------------------------------

/**
 * The set of conditions used in the parser implementation.
 */
 
static bool seq(const char * restrict s, State *state) {
  size_t len = strlen(s);
  for (size_t i = 0; i < len; i++) {
    int32_t c = s[i];
    int32_t c2 = PEEK;
    if (c != c2) return false;
    S_ADVANCE;
  }
  return true;
}
 
#define WS_CASES \
  case ' ': \
  case '\f': \
  case '\n': \
  case '\r': \
  case '\t': \
  case '\v'

/**
 * Require that the next character is whitespace (space or newline) without advancing the parser.
 */
static bool isws(uint32_t c) {
  switch (c) {
    WS_CASES: return true;
    default: return false;
  }
}
 
/**
 * A token like a varsym can be terminated by whitespace or brackets.
 */
static bool token_end(uint32_t c) {
  switch (c) {
    WS_CASES:
    case 0:
    case '(':
    case ')':
    case '[':
    case ']':
      return true;
    default:
      return false;
  }
}
 
/**
 * Require that the argument string follows the current position and is followed by whitespace.
 * See `seq`
 */
static bool token(const char *restrict s, State *state) {
  return seq(s, state) && token_end(PEEK);
}
 
/**
 * Require that the stack of layout indentations is not empty.
 * This is mostly used for safety.
 */
static bool indent_exists(State *state) { return state->indents->len != 0; };
 
/**
 * Require that the current line's indent is greater or equal than the containing layout's, so the current layout is
 * continued.
 */
static bool keep_layout(uint16_t indent, State *state) {
  return indent_exists(state) && indent >= VEC_BACK(state->indents);
}
 
/**
 * Require that the current line's indent is equal to the containing layout's, so the line may start a new `decl`.
 */
static bool same_indent(uint32_t indent, State *state) { return indent_exists(state) && indent == VEC_BACK(state->indents); }
 
/**
 * Require that the current line's indent is smaller than the containing layout's, so the layout may be ended.
 */
static bool smaller_indent(uint32_t indent, State *state) {
  return indent_exists(state) && indent < VEC_BACK(state->indents);
}
 
static bool indent_lesseq(uint32_t indent, State *state) { return indent_exists(state) && indent <= VEC_BACK(state->indents); }

/**
 * Composite condition examining whether the current layout can be terminated if the line after the position where the
 * scan started begins with a `where`.
 *
 * This is needed because `where` can appear on the same indent as, for example, a `do` statement in a `decl`, while
 * being part of the latter and therefore having to end the `do`'s layout before parsing the `where`.
 *
 * This does only check whether the line begins with a `w`, the entire `where` is consumed by the calling parser below.
 */
static bool is_newline_where(uint32_t indent, State *state) {
  return keep_layout(indent, state)
    && (SYM(SEMICOLON) || SYM(END))
    && !SYM(WHERE)
    && PEEK == 'w';
}

#define NEWLINE_CASES \
  case '\n': \
  case '\r': \
  case '\f'

static bool is_newline(uint32_t c) {
  switch (c) {
    NEWLINE_CASES:
      return true;
    default:
      return false;
  }
}

/**
 * NOT NECESSARY IN UNISON. JUST HASKELL.
 * Require that the state has not been initialized after parsing has started.
 *
 * This is necessary to handle a nonexistent `module` declaration.
 */
// static bool uninitialized(State *state) { return !indent_exists(state); }

/**
 * Require that the parser determined an error in the previous step (see `all_syms`).
 */
static bool after_error(State *state) { return all_syms(state->symbols); }

#define SYMBOLICS_WITHOUT_BAR \
    case '!': \
    case '#': \
    case '$': \
    case '%': \
    case '&': \
    case '*': \
    case '+': \
    case '.': \
    case '/': \
    case '<': \
    case '>': \
    case '?': \
    case '^': \
    case ':': \
    case '=': \
    case '-': \
    case '~': \
    case '@': \
    case '\\'

#define SYMBOLIC_CASES \
    SYMBOLICS_WITHOUT_BAR: \
    case '|'

static bool symbolic(uint32_t c) {
  switch (c) {
    SYMBOLIC_CASES:
      return true;
    default:
      return false;
  }
}


typedef enum {
  S_CON,
  S_OP,
  S_SPLICE,
  S_STRICT,
  S_STAR,
  S_TILDE,
  S_IMPLICIT,
  S_MODIFIER,
  S_MINUS,
  S_UNBOXED_TUPLE_CLOSE,
  S_BAR,
  S_COMMENT,
  S_INVALID,
} Symbolic;

// --------------------------------------------------------------------------------------------------------
// Result
// --------------------------------------------------------------------------------------------------------

typedef struct {
    Sym sym;
    bool finished;
} Result;

#ifdef DEBUG
void debug_result(Result res) {
  DEBUG_PRINTF("Result { finished = %d", res.finished);
  if (res.finished) {
    DEBUG_PRINTF(", result = %s }\n", sym_names[res.sym]);
  }
  else
    DEBUG_PRINTF(" }\n");
}
#endif

/**
 * Constructors for the continue, failure and success results.
 */
static Result res_cont = {.sym = FAIL, .finished = false};
static Result res_finish(Sym t) { return (Result) {.sym = t, .finished = true}; }
static Result res_fail = {.sym = FAIL, .finished = true};

// --------------------------------------------------------------------------------------------------------
// Parser
// --------------------------------------------------------------------------------------------------------

/**
 * Parser that terminates the execution with the successful detection of the given symbol.
 */
static Result finish(const Sym s, char *restrict desc) {
  DEBUG_PRINTF("finish: %s\n", desc);
  return res_finish(s);
}

/**
 * Parser that terminates the execution with the successful detection of the given symbol, but only if it is expected.
 */
static Result finish_if_valid(const Sym s, char *restrict desc, State *state) {
  return SYM(s) ? finish(s, desc) : res_cont;
}

/**
 * Add one level of indentation to the stack, caused by starting a layout.
 */
static void push(uint16_t ind, State *state) {
  DEBUG_PRINTF("push: %d\n", ind);
  VEC_PUSH(state->indents, ind);
}

/**
 * Remove one level of indentation from the stack, caused by the end of a layout.
 */
static void pop(State *state) {
  if (indent_exists(state)) {
    DEBUG_PRINTF("pop\n");
    VEC_POP(state->indents);
  }
}

/**
 * Advance the lexer until the following character is neither space nor tab.
 */
static void skipspace(State *state) {
  for (;;) {
    switch (PEEK) {
      case ' ':
      case '\t':
        S_SKIP;
        break;
      default:
        return;
    }
  }
}

static Result layout_end(char *desc, State *state) {
    if(SYM(END)) {
        pop(state);
        return finish(END, desc);
    }
    return res_cont;
}

/**
 * Convenience parser, since those two are often used together.
 */
static Result end_or_semicolon(char *desc, State *state) {
  Result res = layout_end(desc, state);
  SHORT_SCANNER;
  return finish_if_valid(SEMICOLON, desc, state);
}

// --------------------------------------------------------------------------------------------------------
// Logic
// --------------------------------------------------------------------------------------------------------

/**
 * These parsers constitute the higher-level logic, loosely.
 */
 
/**
 * Advance the parser until a non-whitespace character is encountered, while counting whitespace according to the rules
 * in the syntax reference, resetting the counter on each newline.
 *
 * This advances to the first nonwhite character in the next nonempty line and determines its indentation.
 */
static uint32_t count_indent(State *state) {
  uint32_t indent = 0;
  for (;;) {
    switch (PEEK) {
      NEWLINE_CASES:
        S_SKIP;
        indent = 0;
        break;
      case ' ':
        S_SKIP;
        indent++;
        break;
      case '\t':
        S_SKIP;
        indent += 8;
        break;
      default:
        return indent;
    }
  }
}
 
/** End-of-file check.
 *
 * If EOF has been reached, two scenarios are valid:
 *  - The file is empty, in which case the parser is still at the root rule, where `S_EMPTY` is valid.
 *  - The current layout can be ended. This may happen multiple times, since the parser will restart until the last
 *    layout end rule has been parsed.
 * If those cases do not apply, parsing fails.
 */
static Result eof(State *state) {
  if (is_eof(state)) {
    if (SYM(EMPTY)) {
      return finish(EMPTY, "eof");
    }
    Result res = end_or_semicolon("eof", state);
    // return layout_end("eof", state);
    SHORT_SCANNER;
    return res_fail;
  }
  return res_cont;
}
 
 /**
 * NOT NECESSARY IN UNISON. JUST HASKELL.
 * Set the initial indentation at the beginning of the file or module decl to the column of first nonwhite character,
 * then succeed with the dummy symbol `INDENT`.
 *
 * If there is a `module` declaration, this will be handled by the grammar.
 */
// static Result initialize(uint32_t column, State *state) {
  // if (uninitialized(state)) {
  //   MARK("initialize", false, state);
  //   bool match = token("module", state);
  //   if (match) return res_fail;
  //   push(column, state);
  //   return finish(INDENT, "init");
  // }
  // return res_cont;
// }
 
 // NOT NECESSARY IN UNISON. JUST HASKELL.
 // static Result initialize_init(State *state) {
  // if (uninitialized(state)) {
  //   uint32_t col = column(state);
  //   if (col == 0) return initialize(col, state);
  // };
  // return res_cont;
// }
 
/**
 * If a dot is neither preceded nor succeded by whitespace, it may be parsed as a qualified module dot.
 *
 * The preceding space is ensured by sequencing this parser before `skipspace` in `init`.
 * Since this parser cannot look back to see whether the preceding name is a conid, this has to be ensured by the
 * grammar, represented here by the requirement of a valid symbol `DOT`.
 *
 * Since the dot is consumed here, the alternative interpretation, a `VARSYM`, has to be emitted here.
 * A `TYCONSYM` is invalid here, because the dot is only expected in expressions.
 */
static Result dot(State *state) {
  if (SYM(DOT)) {
    if (PEEK == '.') {
      S_ADVANCE;
      if (SYM(VARSYM) && iswspace(PEEK)) return finish(VARSYM, "dot");
      MARK("dot", false, state);
      return finish(DOT, "dot");
    }
  }
  return res_cont;
}
 
 static Result fold(State *state) {
   DEBUG_PRINTF("->fold with PEEK =%c@%u\n", PEEK, column(state));
   if (seq("---", state)) {
     DEBUG_PRINTF("--- and PEEK is %c@%u\n", PEEK, column(state));
     while(!is_eof(state)) S_ADVANCE;
     DEBUG_PRINTF("after advancing, PEEK is %c and should be EOF: %s\n", PEEK, is_eof(state) ? "true" : "false");
     MARK("fold", false, state);
     return finish(FOLD, "fold");
   }
   return res_cont;
 }
 
/**
 * End a layout by removing an indentation from the stack, but only if the current column (which should be in the next
 * line after skipping whitespace) is smaller than the layout indent.
 */
static Result dedent(uint32_t indent, State *state) {
  if (smaller_indent(indent, state)) return layout_end("dedent", state);
  return res_cont;
}
 
/**
 * Succeed if a `where` on a newline can end a statement or layout (see `is_newline_where`).
 *
 * This is the case after `do` or `of`, where the `where` can be on the same indent.
 */
// static Result newline_where(uint32_t indent, State *state) {
//   if (is_newline_where(indent, state)) {
//     MARK("newline_where", false, state);
//     if (token("where", state)) {
//       return end_or_semicolon("newline_where", state);
//     }
//     return res_fail;
//   }
//   return res_cont;
// }
 
/**
 * Succeed for `SEMICOLON` if the indent of the next line is equal to the current layout's.
 */
static Result newline_semicolon(uint32_t indent, State *state) {
  if (SYM(SEMICOLON) && same_indent(indent, state)) {
    return finish(SEMICOLON, "newline_semicolon");
  }
  return res_cont;
}
 
/**
 * End a layout if the next token is an infix operator and the indent is equal to or less than the current layout.
 */
static Result newline_infix(uint32_t indent, Symbolic type, State *state) {
  // if (end_on_infix(indent, type, state)) {
  //   return layout_end("newline_infix", state);
  // }
  return res_cont;
}
 
/**
 * Parse an inline `where` token.
 *
 * Necessary because `is_newline_where` needs to know that no `where` may follow.
 */
static Result where(State *state) {
  if (token("where", state)) {
    if (SYM(WHERE)) {
      MARK("where", false, state);
      return finish(WHERE, "where");
    }
    return layout_end("where", state);
  }
  return res_cont;
}
 
/**
 * An `in` token ends the layout openend by a `let` and its nested layouts.
 */
static Result in(State *state) {
  if (SYM(IN) && token("in", state)) {
    MARK("in", false, state);
    pop(state);
    return finish(IN, "in");
  }
  return res_cont;
}

/**
 * An `else` token may end a layout opened in the body of a `then`.
 */
static Result else_(State *state) {
  return token("else", state) ? layout_end("else", state) : res_cont;
}

/**
 * Consume all characters up to the end of line and succeed with `syms::commment`.
 */
static Result inline_comment(State *state) {
  DEBUG_PRINTF("->inline comment\n");
  for (;;) {
    DEBUG_PRINTF("Examining if still same line: %c\n", PEEK);
    switch (PEEK) {
      NEWLINE_CASES:
      case 0:
        goto inline_comment_after_skip;
      default:
        S_ADVANCE;
        break;
    }
  }

inline_comment_after_skip:
  MARK("inline_comment", false, state);
  return finish(COMMENT, "inline_comment");
}

/**
 * Parse an inline comment if the next chars are two or more minuses and the char after the last minus is not
 * symbolic.
 *
 * To be called when it is certain that two (or three!) minuses cannot succeed as a symbolic operator.
 * Those cases are:
 *   - `START` is valid
 *   - Operator matching was done already
 */
static Result minus(State *state) {
  DEBUG_PRINTF("->minus\n");
  if (!seq("--", state)) return res_cont;
  DEBUG_PRINTF("Col: %u; Peek: %c\n", column(state), PEEK);
  if (PEEK == '-') {
    // if (SYM(FOLD)) {
      S_ADVANCE;
      DEBUG_PRINTF("After advancing, PEEK: %c\n", PEEK);
      if (is_eof(state) || is_newline(PEEK)) {
        while(!is_eof(state)) S_ADVANCE;
        MARK("minus", false, state);
        return finish(FOLD, "fold");
      } else {
        return res_fail;
      }
    // }
  } 
  // while (PEEK == '-') S_ADVANCE;
  // if (symbolic(PEEK)) return res_fail;
  return inline_comment(state);
}

/**
 * Succeed for a comment.
 */
static Result multiline_comment_success(State *state) {
  MARK("multiline_comment", false, state);
  return finish(COMMENT, "multiline_comment");
}

/**
 * See `nested_comment`.
 *
 * Since {- -} comments can be nested arbitrarily, this has to keep track of how many have been openend, so that the
 * outermost comment isn't closed prematurely.
 */
static Result multiline_comment(State *state) {
  uint16_t level = 0;
  for (;;) {
    switch (PEEK) {
      case '{':
        S_ADVANCE;
        if (PEEK == '-') {
          S_ADVANCE;
          level++;
        }
        break;
      case '-':
        S_ADVANCE;
        if (PEEK == '}') {
          S_ADVANCE;
          if (level == 0) return multiline_comment_success(state);
          level--;
        }
        break;
      case 0: {
        Result res = eof(state);
        SHORT_SCANNER;
        return res_fail;
      }
      default:
        S_ADVANCE;
        break;
    }
  }
}

/**
 * NOTE: No pragmas in Unison; this is a Haskell thing. Remove the {-#
 * When a brace is encountered, it can be an explicitly started layout, a pragma, or a comment. In the latter case, the
 * comment is parsed, otherwise parsing fails to delegate to the corresponding grammar rule.
 */
static Result brace(State *state) {
  if (PEEK != '{') return res_fail;
  S_ADVANCE;
  if (PEEK != '-') return res_fail;
  S_ADVANCE;
  // if (PEEK == '#') return res_fail;
  return multiline_comment(state);
}


/**
 * Parse either inline or block comments. (or fold)
 */
static Result comment(State *state) {
  DEBUG_PRINTF("->comment w/ PEEK = %c\n", PEEK);
  switch (PEEK) {
    case '-': {
      Result res = minus(state);
      SHORT_SCANNER;
      return res_fail;
    }
    case '{': {
      Result res = brace(state);
      SHORT_SCANNER;
      return res_fail;
    }
  }
  return res_cont;
}
 
static Result close_layout_in_list(State *state) {
  switch (PEEK) {
    case ']': {
      if (state->symbols[END]) {
        pop(state);
        return finish(END, "bracket");
      }
      break;
    }
    case ',': {
      S_ADVANCE;
      if (state->symbols[COMMA]) {
        MARK("comma", false, state);
        return finish(COMMA, "comma");
      }
      Result res = layout_end("comma", state);
      SHORT_SCANNER;
      return res_fail;
    }
  }
  return res_cont;
}

/** Parse special tokens before the first newline that can't be reliably detected by tree-sitter:
 *
 *   - `where` here is just for the actual valid token
 *   - `in` closes a layout when inline
 *   - `)` can end the layout of an `of`
 *   - symbolic operators are complicated to implement with regex
 *   - `$` can be a splice if not followed by whitespace
 *   - '[' can be a list or a quasiquote
 *   - '|' in a quasiquote, since it can be followed by symbolic operator characters, which would be consumed
 */
static Result inline_tokens(State *state) {
  switch (PEEK) {
    case 'w': {
      Result res = where(state);
      SHORT_SCANNER;
      return res_fail;
    }
    case 'i': {
      Result res = in(state);
      SHORT_SCANNER;
      return res_fail;
    }
    case 'e': {
      Result res = else_(state);
      SHORT_SCANNER;
      return res_fail;
    }
    case ')': {
      Result res = layout_end(")", state);
      SHORT_SCANNER;
      return res_fail;
    }
    // TODO(414owen) does this clash with inline comments '--'?
    // I'm not sure why there's a `symbolic::comment` and a `COMMENT`...
    // SYMBOLICS_WITHOUT_BAR: {
    //   Symbolic s = read_symop(state);
    //   return symop(s, state);
    // }
    // case '|': {
    //   if (state->symbols[QQ_BAR]) {
    //     S_ADVANCE;
    //     MARK("qq_bar", false, state);
    //     return res_finish(QQ_BAR);
    //   }
    //   Symbolic s = read_symop(state);
    //   return symop(s, state);
    // }
    // case '[': {
    //   if (state->symbols[QQ_START]) {
    //     S_ADVANCE;
    //     Result res = qq_start(state);
    //     SHORT_SCANNER;
    //   }
    //   return res_fail;
    // }
    // '-' case covered by symop
    // case '{': {
    //   Result res = comment(state);
    //   SHORT_SCANNER;
    // }
  }
  return close_layout_in_list(state);
}

/**
 * If the symbol `START` is valid, starting a new layout is almost always indicated.
 *
 * If the next character is a left brace, it is either a comment, pragma or an explicit layout. In the comment case, the
 * it must be parsed here.
 * If the next character is a minus, it might be a comment.
 *
 * In all of those cases, the layout can't be started now. In the comment and pragma case, it will be started in the
 * next run.
 *
 * This pushes the indentation of the first non-whitespace character onto the stack.
 */
static Result layout_start(uint32_t column, State *state) {
    if (state->symbols[START]) {
        switch (PEEK) {
            
        }
        push(column, state);
        return finish(START, "layout_start");
    }
    return res_cont;
}

/**
 * After a layout has ended, the originator might need to be terminated by semicolon as well, but since the layout end
 * advances until the next line, it cannot be done in the newline checks.
 *
 * This can happen, for example, with nested `do` layouts:
 *
 * f = do
 *   a <- b
 *   do c <- d
 *      e
 *   f
 *
 * Here, when the inner `do`'s  layout is ended, the next step is started at `f`, but the outer `do`'s layout expects a
 * semicolon. Since `f` is on the same indent as the outer `do`'s layout, this parser matches.
 */
static Result post_end_semicolon(uint32_t column, State *state) {
  DEBUG_PRINTF("->post end semicolon\n");
  return SYM(SEMICOLON) && indent_lesseq(column, state)
    ? finish(SEMICOLON, "post_end_semicolon")
    : res_cont;
}

/** 
 * Like `post_end_semicolon`, but for layout end.
 */
static Result repeat_end(uint32_t column, State *state) {
  if (state->symbols[END] && smaller_indent(column, state)) {
    return layout_end("repeat_end", state);
  }
  return res_cont;
}
 
/**
 * Rules that decide based on the indent of the next line.
 */
static Result newline_indent(uint32_t indent, State *state) {
  Result res = dedent(indent, state);
  SHORT_SCANNER;
  res = close_layout_in_list(state);
  SHORT_SCANNER;
  return newline_semicolon(indent, state);
}
 
/**
 * Rules that decide based on the first token on the next line.
 */
static Result newline_token(uint32_t indent, State *state) {
  DEBUG_PRINTF("->newline_token\n");
  if (PEEK == '-') {
    return minus(state);
  }
  switch (PEEK) {
    SYMBOLIC_CASES:
    case '`': {
      // Symbolic s = read_symop(state);
      // Result res = newline_infix(indent, s, state);
      // SHORT_SCANNER;
      return res_fail;
    }
  }
  // NOTE: "where" cannot begin a new line in Unison, just Haskell
  // Result res = newline_where(indent, state);
  // SHORT_SCANNER;
  if (PEEK == 'i') return in(state);
  return res_cont;
}
 
/**
 * To be called after parsing a newline, with the indent of the next line as argument.
 */
static Result newline(uint32_t indent, State *state) {
  DEBUG_PRINTF("->newline(%u)\n", indent);
  Result res = eof(state);
  SHORT_SCANNER;
  // res = initialize(indent, state);
  // SHORT_SCANNER;
  // res = cpp(state);
  // SHORT_SCANNER;
  res = comment(state);
  SHORT_SCANNER;
  res = newline_token(indent, state);
  SHORT_SCANNER;
  return newline_indent(indent, state);
}
 
/**
 * Parsers that have to run when the next non-space character is not a newline:
 *
 *   - Layout start
 *   - ending nested layouts at the same position
 *   - symbolic operators
 *   - Tokens `where`, `in`, `$`, `)`, `]`, `,`
 *   - comments
 */
static Result immediate(uint32_t column, State *state) {
  Result res = layout_start(column, state);
  SHORT_SCANNER;
  res = post_end_semicolon(column, state);
  SHORT_SCANNER;
  res = repeat_end(column, state);
  SHORT_SCANNER;
  return inline_tokens(state);
}

/**
 * Parsers that have to run _before_ parsing whitespace:
 *
 *   - Error check
 *   - Indent stack initialization
 *   - Qualified module dot (leading whitespace would mean it would be `(.)`)
 *   - Fold
 */
static Result init(State *state) {
  DEBUG_PRINTF("->init\n");
  Result res = eof(state);
  SHORT_SCANNER;
  res = after_error(state) ? res_fail : res_cont;
  #ifdef DEBUG
  debug_result(res);
  #endif
  SHORT_SCANNER;
  // res = initialize_init(state);
  // SHORT_SCANNER;
  res = dot(state);
  SHORT_SCANNER;  
  if (SYM(FOLD)) {
    res = fold(state);
    SHORT_SCANNER;
  }
  
  // res = cpp(state);
  // SHORT_SCANNER;
  // if (state->symbols[QQ_BODY]) {
    // return qq_body(state);
  // }
  return res_cont;
}

/**
 * The main parser checks whether the first non-space character is a newline and delegates accordingly.
 */
static Result scan_main(State *state) {
  DEBUG_PRINTF("->scan_main w/PEEK = %c (%d)\n", PEEK, PEEK);
  skipspace(state);
  Result res = eof(state);
  SHORT_SCANNER;
  DEBUG_PRINTF("Not eof\n");
  MARK("main", false, state);
  if (is_newline(PEEK)) {
    DEBUG_PRINTF("is newline\n");
    S_SKIP;
    uint32_t indent = count_indent(state);
    return newline(indent, state);
  }
  uint32_t col = column(state);
  return immediate(col, state);
}

/**
 * The entry point to the parser.
 */
static Result scan_all(State *state) {
  // DEBUG_PRINTF("->scan_all\n");
  Result res = init(state);
  // if (res.finished) {
    // DEBUG_PRINTF("after init, scan_all is: %s\n", sym_names[res.sym]);
  // }
  SHORT_SCANNER;
  return scan_main(state);
}

// --------------------------------------------------------------------------------------------------------
// Evaluation
// --------------------------------------------------------------------------------------------------------

/**
  * Helper that consume_if all characters up to the next whitespace, for debugging after a run.
  *
  * Note: This may break the parser, since not all paths use `mark`.
  */
#ifdef DEBUG
static void debug_lookahead(State *state) {
  bool first = true;
  for (;;) {
    if (isws(PEEK) || PEEK == 0) break;
    else {
      if (first) DEBUG_PRINTF("next: ");
      DEBUG_PRINTF("%c\n", PEEK);
      S_ADVANCE;
      first = false;
    }
  }
}
#endif

/**
  * The main function of the parsing machinery, executing the parser by passing in the initial state and analyzing the
  * result.
  *
  * If the parser concluded with success, the `result_symbol` attribute of the lexer is set, by which the parsed symbol
  * is communicated to tree-sitter, and `true` is returned, indicating to tree-sitter to use the result.
  *
  * If the parser concluded with failure, no `result_symbol` is set and `false` is returned.
  *
  * If the parser did _not_ conclude, i.e. all steps finished with `cont`, a failure is reported as well.
  *
  * If the `DEBUG_NEXT_TOKEN` flag is set, the next token will be printed.
  */
static bool eval(Result (*chk)(State *state), State *state) {
  Result result = chk(state);
#ifdef DEBUG_NEXT_TOKEN
  debug_lookahead(state);
#endif
  if (result.finished && result.sym != FAIL) {
#ifdef DEBUG
    // TODO(414owen) can names[] fail?
    DEBUG_PRINTF("result: %s, ", sym_names[result.sym]);
    if (state->marked == -1) {
      DEBUG_PRINTF("%d\n", column(state));
    } else {
      DEBUG_PRINTF("%s@%d\n", state->marked_by, state->marked);
    }
#endif
    state->lexer->result_symbol = result.sym;
    DEBUG_PRINTF("Lexer result: %s", sym_names[state->lexer->result_symbol]);
    return true;
  } else return false;
}

// ----------
// API
// ----------

/**
 * This function allocates the persistent state of the parser that is passed into the other API functions.
 */
void *tree_sitter_unison_external_scanner_create() {
  void *res = calloc(sizeof(indent_vec), 1);
  return res;
}

/**
 * Main logic entry point.
 * Since the state is a singular vector, it can just be cast and used directly.
 */
bool tree_sitter_unison_external_scanner_scan(void *indents_v, TSLexer *lexer, const bool *syms) {
  indent_vec *indents = (indent_vec*) indents_v;
  State state = {
    .lexer = lexer,
    .symbols = syms,
    .indents = indents
  };
  DEBUG_PRINTF("===================\nBeginning scanner\n");
#ifdef DEBUG
  debug_state(&state);
  DEBUG_PRINTF("PEEK: %c\n", state.lexer->lookahead);
  if (state.needs_free) free(state.marked_by);
#endif
  bool res = eval(scan_all, &state);
  DEBUG_PRINTF("End scanner with %s and symbol %s\n", res ? "success" : "failure", state.lexer->result_symbol ? sym_names[state.lexer->result_symbol] : "(none)");
  return res;
}

/**
 * Copy the current state to another location for later reuse.
 * This is normally more complex, but since this parser's state constists solely of a vector of integers, it can just be
 * copied.
 */
unsigned tree_sitter_unison_external_scanner_serialize(void *indents_v, char *buffer) {
  indent_vec *indents = (indent_vec*) indents_v;
  unsigned to_copy = sizeof(indents->data[0]) * indents->len;
  if (to_copy > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    return 0;
  }
  memcpy(buffer, indents->data, to_copy);
  return to_copy;
}

/**
 * Load another parser state into the currently active state.
 * `payload` is the state of the previous parser execution, while `buffer` is the saved state of a different position
 * (e.g. when doing incremental parsing).
 */
void tree_sitter_unison_external_scanner_deserialize(void *indents_v, char *buffer, unsigned length) {
  indent_vec *indents = (indent_vec*) indents_v;
  unsigned els = length / sizeof(indents->data[0]);
  if (els > 0) {
    VEC_GROW(indents, els);
    indents->len = els;
    memcpy(indents->data, buffer, length);
  }
}

/**
 * Destroy the state.
 */
void tree_sitter_unison_external_scanner_destroy(void *indents_v) {
  indent_vec *indents = (indent_vec*) indents_v;
  VEC_FREE(indents);
}