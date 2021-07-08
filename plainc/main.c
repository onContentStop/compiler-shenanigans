#include <bitset.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vec.h>

#define bitset_equals(b1, b2)                                                  \
  bitset_intersection_count(b1, b2) == bitset_count(b1)

#define EDGE_EMPTY (-3)
#define EDGE_CHARACTER_CLASS (-2)
#define EDGE_EPSILON 0

#define ANCHOR_NONE 0
#define ANCHOR_BOL (1 << 0)
#define ANCHOR_EOL (1 << 1)
#define ANCHOR_BOTH (ANCHOR_BOL | ANCHOR_EOL)

typedef struct nfa_node_t {
  struct nfa_node_t *next[2];
  int edge;
  bitset_t *bitset;
  bool complement;
  int anchor;
  int index;
} nfa_node_t;

typedef vec_t(nfa_node_t *) vec_nfa_node_t;

typedef struct {
  vec_nfa_node_t nfa;
  size_t start;
} nfa_t;

static nfa_t thompson(const char *input);
static void nfa_print(nfa_t *nfa);

typedef enum {
  tok_eoi,
  tok_left_bracket,
  tok_right_bracket,
  tok_left_paren,
  tok_right_paren,
  tok_carat,
  tok_dash,
  tok_dot,
  tok_dollar,
  tok_literal,
  tok_pipe,
  tok_plus,
  tok_question_mark,
  tok_star,
} regex_token_t;

typedef struct {
  vec_nfa_node_t nfa;
  vec_int_t discard_stack;
  const char *input;
  const char *input_start;
  regex_token_t current_token;
  char current_lexeme;
  bool in_quote;
} nfa_parser_state_t;

static nfa_node_t *alloc_nfa(nfa_parser_state_t *state) {
  if (state->discard_stack.length == 0) {
    nfa_node_t *node = malloc(sizeof(nfa_node_t));
    memset(node, 0, sizeof(nfa_node_t));
    node->bitset = bitset_create();
    node->index = state->nfa.length;
    vec_push(&state->nfa, node);
    return node;
  }

  int discarded = vec_pop(&state->discard_stack);
  state->nfa.data[discarded] = malloc(sizeof(nfa_node_t));
  nfa_node_t *node = state->nfa.data[discarded];
  memset(node, 0, sizeof(nfa_node_t));
  node->bitset = bitset_create();
  node->index = discarded;
  return node;
}

static void discard_nfa(nfa_parser_state_t *state, nfa_node_t *node) {
  int index = node->index;
  vec_push(&state->discard_stack, node->index);
  free(node);
  state->nfa.data[index] = NULL;
}

static regex_token_t regex_token_from_char(char c) {
  switch (c) {
  case '$':
    return tok_dollar;
  case '(':
    return tok_left_paren;
  case ')':
    return tok_right_paren;
  case '*':
    return tok_star;
  case '+':
    return tok_plus;
  case '-':
    return tok_dash;
  case '.':
    return tok_dot;
  case '?':
    return tok_question_mark;
  case '[':
    return tok_left_bracket;
  case ']':
    return tok_right_bracket;
  case '^':
    return tok_carat;
  case '|':
    return tok_pipe;
  default:
    return tok_literal;
  }
}

static void nfa_parser_state_init(nfa_parser_state_t *state,
                                  const char *input) {
  vec_init(&state->nfa);
  vec_init(&state->discard_stack);
  state->current_lexeme = '\0';
  state->current_token = tok_eoi;
  state->in_quote = false;
  state->input = input;
  state->input_start = input;
}

static char esc(const char **input) {
  if (**input == '\\') {
    ++*input;
    switch (**input) {
    case 't':
      return '\t';
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    }
  }
  return **input;
}

static regex_token_t advance(nfa_parser_state_t *state) {
  if (*state->input == '\0') {
    state->current_token = tok_eoi;
    state->current_lexeme = '\0';
    return tok_eoi;
  }
  if (*state->input == '"') {
    state->in_quote = !state->in_quote;
    ++state->input;
    if (!*state->input) {
      state->current_token = tok_eoi;
      state->current_lexeme = '\0';
      return tok_eoi;
    }
  }
  bool saw_esc = *state->input == '\\';
  if (!state->in_quote) {
    state->current_lexeme = esc(&state->input);
    ++state->input;
  } else {
    if (saw_esc && state->input[1] == '"') {
      state->input += 2;
      state->current_lexeme = '"';
    } else {
      state->current_lexeme = *state->input;
      ++state->input;
    }
  }
  state->current_token = (state->in_quote || saw_esc)
                             ? tok_literal
                             : regex_token_from_char(state->current_lexeme);
  return state->current_token;
}

static void cat_expr(nfa_parser_state_t *state, nfa_node_t **sptr,
                     nfa_node_t **eptr);
static void do_dash(nfa_parser_state_t *state, bitset_t *set);
static void expr(nfa_parser_state_t *state, nfa_node_t **sptr,
                 nfa_node_t **eptr);
static void factor(nfa_parser_state_t *state, nfa_node_t **sptr,
                   nfa_node_t **eptr);
static bool first_in_cat(regex_token_t token);
static nfa_node_t *machine(nfa_parser_state_t *state);
static nfa_node_t *rule(nfa_parser_state_t *state);
static void term(nfa_parser_state_t *state, nfa_node_t **sptr,
                 nfa_node_t **eptr);
static void nfa_print(nfa_t *nfa);

static nfa_node_t *machine(nfa_parser_state_t *state) {
  nfa_node_t *start = alloc_nfa(state);
  nfa_node_t *p = start;
  advance(state);
  p->next[0] = rule(state);
  while (state->current_token != tok_eoi) {
    p->next[1] = alloc_nfa(state);
    p = p->next[1];
    p->next[0] = rule(state);
  }
  return start;
}

static nfa_node_t *rule(nfa_parser_state_t *state) {
  nfa_node_t *start = NULL;
  nfa_node_t *end = NULL;
  int anchor = ANCHOR_NONE;
  if (state->current_token == tok_carat) {
    start = alloc_nfa(state);
    start->edge = '\n';
    anchor |= ANCHOR_BOL;
    advance(state);
    expr(state, &start->next[0], &end);
  } else {
    expr(state, &start, &end);
  }

  if (state->current_token == tok_dollar) {
    advance(state);
    end->next[0] = alloc_nfa(state);
    end->edge = EDGE_CHARACTER_CLASS;
    end->bitset = bitset_create();
    bitset_set(end->bitset, '\n');
    bitset_set(end->bitset, '\r');
    end = end->next[0];
    anchor |= ANCHOR_EOL;
  }

  end->anchor = anchor;
  advance(state);
  return start;
}

static void expr(nfa_parser_state_t *state, nfa_node_t **sptr,
                 nfa_node_t **eptr) {
  cat_expr(state, sptr, eptr);
  while (state->current_token == tok_pipe) {
    advance(state);
    nfa_node_t *e2_start;
    nfa_node_t *e2_end;
    cat_expr(state, &e2_start, &e2_end);
    nfa_node_t *p = alloc_nfa(state);
    p->next[1] = e2_start;
    p->next[0] = *sptr;
    *sptr = p;
    p = alloc_nfa(state);
    (*eptr)->next[0] = p;
    e2_end->next[0] = p;
    *eptr = p;
  }
}

static void cat_expr(nfa_parser_state_t *state, nfa_node_t **sptr,
                     nfa_node_t **eptr) {
  if (first_in_cat(state->current_token)) {
    factor(state, sptr, eptr);
  }
  while (first_in_cat(state->current_token)) {
    nfa_node_t *e2_start;
    nfa_node_t *e2_end;
    factor(state, &e2_start, &e2_end);
    memcpy(*eptr, e2_start, sizeof(nfa_node_t));
    discard_nfa(state, e2_start);
    *eptr = e2_end;
  }
}

static bool first_in_cat(regex_token_t token) {
  switch (token) {
  case tok_right_paren:
  case tok_dollar:
  case tok_pipe:
  case tok_eoi:
    return false;
  case tok_star:
    fprintf(stderr, "'*' must follow an expression.\n");
    exit(1);
  case tok_plus:
    fprintf(stderr, "'+' must follow an expression.\n");
    exit(1);
  case tok_question_mark:
    fprintf(stderr, "'?' must follow an expression.\n");
    exit(1);
  case tok_right_bracket:
    fprintf(stderr, "encountered a stray ']'\n");
    exit(1);
  case tok_carat:
    fprintf(stderr, "encountered a stray '^'\n");
    exit(1);
  default:
    return true;
  }
}

static void factor(nfa_parser_state_t *state, nfa_node_t **sptr,
                   nfa_node_t **eptr) {
  term(state, sptr, eptr);
  if (state->current_token == tok_star || state->current_token == tok_plus ||
      state->current_token == tok_question_mark) {
    nfa_node_t *start = alloc_nfa(state);
    nfa_node_t *end = alloc_nfa(state);
    start->next[0] = *sptr;
    (*eptr)->next[0] = end;
    if (state->current_token == tok_star ||
        state->current_token == tok_question_mark) {
      start->next[1] = end;
    }
    if (state->current_token == tok_star || state->current_token == tok_plus) {
      (*eptr)->next[1] = *sptr;
    }
    *sptr = start;
    *eptr = end;
    advance(state);
  }
}

static void term(nfa_parser_state_t *state, nfa_node_t **sptr,
                 nfa_node_t **eptr) {
  if (state->current_token == tok_left_paren) {
    advance(state);
    expr(state, sptr, eptr);
    if (state->current_token == tok_right_paren) {
      advance(state);
    } else {
      fprintf(stderr, "Expected ')'");
      exit(1);
    }
  } else {
    nfa_node_t *start = alloc_nfa(state);
    *sptr = start;
    *eptr = start->next[0] = alloc_nfa(state);
    if (state->current_token != tok_dot &&
        state->current_token != tok_left_bracket) {
      start->edge = state->current_lexeme;
      advance(state);
    } else {
      start->edge = EDGE_CHARACTER_CLASS;
      start->bitset = bitset_create();
      if (state->current_token == tok_dot) {
        bitset_set(start->bitset, '\n');
        bitset_set(start->bitset, '\r');
        start->complement = true;
      } else {
        advance(state);
        if (state->current_token == tok_carat) {
          advance(state);
          bitset_set(start->bitset, '\n');
          bitset_set(start->bitset, '\r');
          start->complement = true;
        }
        if (state->current_token != tok_right_bracket) {
          do_dash(state, start->bitset);
        } else {
          for (char c = 0; c <= ' '; ++c) {
            bitset_set(start->bitset, c);
          }
        }
      }
      advance(state);
    }
  }
}

static void do_dash(nfa_parser_state_t *state, bitset_t *bitset) {
  register int first;
  for (; state->current_token != tok_eoi &&
         state->current_token != tok_right_bracket;
       advance(state)) {
    if (state->current_token != tok_dash) {
      first = state->current_lexeme;
      bitset_set(bitset, first);
    } else {
      advance(state);
      for (; first <= state->current_lexeme; ++first) {
        bitset_set(bitset, first);
      }
    }
  }
}

static void ccl_print(bitset_t *set) {
  putchar('[');
  for (int i = 0; i < 0x7F; ++i) {
    if (bitset_get(set, i)) {
      if (i < ' ') {
        printf("^%c", i + '@');
      } else {
        printf("%c", i);
      }
    }
  }
  putchar(']');
}

static void nfa_print(nfa_t *nfa) {
  for (int i = 0; i < nfa->nfa.length; i++) {
    printf("NFA state %02d: ", nfa->nfa.data[i]->index);
    if (nfa->nfa.data[i]->next[0] == NULL) {
      printf("(TERMINAL)");
    } else {
      printf("--> %02d ", nfa->nfa.data[i]->next[0]->index);
      printf("(%02d) on ",
             nfa->nfa.data[i]->next[1] ? nfa->nfa.data[i]->next[1]->index : -1);
      switch (nfa->nfa.data[i]->edge) {
      case EDGE_CHARACTER_CLASS:
        ccl_print(nfa->nfa.data[i]->bitset);
        break;
      case EDGE_EPSILON:
        printf("EPSILON ");
        break;
      default:
        printf("'%c'", nfa->nfa.data[i]->edge);
        break;
      }
    }

    if (i == nfa->start) {
      printf(" (START STATE)");
    }
    printf("\n");
  }
}

static nfa_t thompson(const char *input) {
  nfa_parser_state_t state;
  state.input = input;
  state.input_start = input;
  vec_init(&state.nfa);
  state.in_quote = false;
  vec_init(&state.discard_stack);
  nfa_t out;
  out.start = machine(&state)->index;
  out.nfa = state.nfa;
  for (int i = 0; i < out.nfa.length; ++i) {
    if (out.nfa.data[i]) {
      out.nfa.data[i]->index = i;
    }
  }
  return out;
}

void nfa_free(nfa_t *nfa) {
  for (int i = 0; i < nfa->nfa.length; ++i) {
    if (nfa->nfa.data[i]) {
      free(nfa->nfa.data[i]);
    }
  }
  vec_deinit(&nfa->nfa);
}

typedef vec_t(bitset_t *) vec_bitset_t;

typedef struct dfa_node_t {
  bitset_t *bitset;
  char id;
  vec_t(struct dfa_node_t *) next;
  vec_bitset_t chars;
  int partition;
  int index;
} dfa_node_t;

static dfa_node_t *epsilon_closure(nfa_t *nfa, bitset_t *input) {
  vec_int_t stack;
  vec_init(&stack);

  for (int i = 0; i < nfa->nfa.length; ++i) {
    if (bitset_get(input, i)) {
      vec_push(&stack, i);
    }
  }

  while (stack.length > 0) {
    int i = vec_pop(&stack);
    nfa_node_t *p = nfa->nfa.data[i];
    if (p->edge == EDGE_EPSILON) {
      for (int j = 0; j <= 1; ++j) {
        if (p->next[j]) {
          i = p->next[j]->index;
          if (!bitset_get(input, i)) {
            bitset_set(input, i);
            vec_push(&stack, i);
          }
        }
      }
    }
  }
  dfa_node_t *dfa_node = malloc(sizeof(dfa_node_t));
  dfa_node->bitset = input;
  vec_init(&dfa_node->next);
  vec_init(&dfa_node->chars);
  return dfa_node;
}

static dfa_node_t *move(nfa_t *nfa, bitset_t *input, char c) {
  bitset_t *outset = NULL;
  for (int i = nfa->nfa.length - 1; i >= 0; --i) {
    if (bitset_get(input, i)) {
      nfa_node_t *p = nfa->nfa.data[i];
      if (p->edge == c || p->edge == EDGE_CHARACTER_CLASS &&
                              (p->complement != bitset_get(p->bitset, c))) {
        if (!outset) {
          outset = bitset_create();
        }
        bitset_set(outset, p->next[0]->index);
      }
    }
  }
  dfa_node_t *dfa_node = malloc(sizeof(dfa_node_t));
  dfa_node->bitset = outset;
  vec_init(&dfa_node->next);
  vec_init(&dfa_node->chars);
  return dfa_node;
}

typedef vec_t(dfa_node_t *) dfa_t;

static dfa_t nfa_to_dfa(nfa_t *nfa) {
  bitset_t *init = bitset_create();
  bitset_set(init, nfa->start);
  dfa_node_t *d0 = epsilon_closure(nfa, init);
  dfa_t dfa;
  dfa_t work;
  vec_init(&dfa);
  vec_init(&work);
  vec_push(&dfa, d0);
  vec_push(&work, d0);
  char id = 'A';
  while (work.length > 0) {
    dfa_node_t *di = vec_pop(&work);
    di->id = id;
    for (char c = 1; c < 0x7F; ++c) {
      dfa_node_t *dj = move(nfa, di->bitset, c);
      if (dj->bitset) {
        dfa_node_t *s = epsilon_closure(nfa, dj->bitset);
        free(dj);
        dj = s;
        bool unique = true;
        bool in_next = false;
        for (int i = 0; i < dfa.length; ++i) {
          if (bitset_equals(dfa.data[i]->bitset, dj->bitset)) {
            unique = false;
            for (int j = 0; j < di->next.length; ++j) {
              if (di->next.data[j] == dfa.data[i]) {
                in_next = true;
                bitset_set(di->chars.data[j], c);
                break;
              }
            }
            if (!in_next) {
              vec_push(&di->next, dfa.data[i]);
            }
            dj->id = i + 'A';
            break;
          }
        }
        if (!in_next) {
          bitset_t *b = bitset_create();
          bitset_set(b, c);
          vec_push(&di->chars, b);
        }
        if (unique) {
          vec_push(&di->next, dj);
          vec_push(&dfa, dj);
          vec_push(&work, dj);
        } else {
          free(dj);
        }
      } else {
        // final state
        free(dj);
      }
    }
    ++id;
  }
  for (int i = 0; i < dfa.length; ++i) {
    dfa.data[i]->index = i;
  }
  return dfa;
}

static void dfa_to_dot(const dfa_t *dfa) {
  printf("digraph test {\n");
  for (int i = 0; i < dfa->length; ++i) {
    const dfa_node_t *di = dfa->data[i];
    for (int j = 0; j < di->next.length; ++j) {
      const dfa_node_t *dj = di->next.data[j];
      printf("%c -> %c [ label = \"'", di->id, dj->id);
      bitset_t *b = di->chars.data[j];
      for (char c = 0; c < 0x7F; ++c) {
        if (bitset_get(b, c)) {
          if (c == '\'' || c == '"' || c == '\\') {
            printf("\\");
          }
          if (c < ' ') {
            printf("^%c", c + '@');
          } else {
            printf("%c", c);
          }
        }
      }
      printf("'\" ]\n");
    }
    // if (di->next.length == 0) {
    //   printf("%c is an accepting state\n", di->id);
    // }
  }
  printf("}\n");
}

typedef vec_t(dfa_node_t *) partition_t;
typedef vec_t(partition_t *) vec_partition_t;

static dfa_node_t *do_goto(dfa_node_t *node, char c) {
  for (int i = 0; i < node->chars.length; ++i) {
    if (bitset_get(node->chars.data[i], c)) {
      return node->next.data[i];
    }
  }
  return NULL;
}

static dfa_t minimize_dfa(dfa_t *dfa) {
  // accepting, nonaccepting
  // for each partition:
  //  for each state in the partition:
  //   move all states that are not equivalent to the first
  //   to a new partition
  vec_partition_t partitions;
  vec_init(&partitions);
  partition_t *accepting = malloc(sizeof(partition_t));
  vec_init(accepting);
  partition_t *nonaccepting = malloc(sizeof(partition_t));
  vec_init(nonaccepting);
  for (int i = 0; i < dfa->length; ++i) {
    if (dfa->data[i]->next.length == 0) {
      dfa->data[i]->partition = 0;
      vec_push(accepting, dfa->data[i]);
    } else {
      dfa->data[i]->partition = 1;
      vec_push(nonaccepting, dfa->data[i]);
    }
  }
  vec_push(&partitions, accepting);
  vec_push(&partitions, nonaccepting);

  for (int i = 0; i < partitions.length; ++i) {
    partition_t *new_partition = NULL;
    vec_int_t removals;
    vec_init(&removals);
    partition_t *pi = partitions.data[i];
    printf("partition %d = ", i);
    for (int j = 0; j < pi->length; ++j) {
      if (!pi->data[j]) {
        continue;
      }
      printf("%c ", pi->data[j]->id);
    }
    printf("\n");
    dfa_node_t *first = pi->data[0];
    for (int j = 1; j < pi->length; ++j) {
      dfa_node_t *dij = pi->data[j];
      if (!dij) {
        continue;
      }
      for (char c = 0; c < 0x7F; ++c) {
        dfa_node_t *goto_first = do_goto(first, c);
        dfa_node_t *goto_dij = do_goto(dij, c);
        bool remove = false;
        if ((goto_first == NULL) ^ (goto_dij == NULL)) {
          remove = true;
        }
        if (goto_first != NULL && goto_dij != NULL &&
            goto_first->partition != goto_dij->partition) {
          remove = true;
        }
        if (remove) {
          if (!new_partition) {
            new_partition = malloc(sizeof(partition_t));
            vec_init(new_partition);
          }
          vec_push(new_partition, dij);
          dij->partition = partitions.length;
          vec_push(&removals, j);
          break;
        }
      }
    }
    if (new_partition) {
      vec_push(&partitions, new_partition);
    }
    for (int j = 0; j < removals.length; ++j) {
      int removal = removals.data[j];
      // no need to free, the node is in the new partition
      pi->data[j] = NULL;
    }

    printf("partition %d = ", i);
    for (int j = 0; j < pi->length; ++j) {
      if (!pi->data[j]) {
        continue;
      }
      printf("%c ", pi->data[j]->id);
    }
    printf("\n");
  }
  dfa_t result;
  vec_init(&result);
  char id = 'A';
  vec_t(vec_int_t *) pointers;
  vec_init(&pointers);
  for (int i = 0; i < partitions.length; ++i) {
    vec_int_t *next = malloc(sizeof(vec_int_t));
    vec_init(next);
    dfa_node_t *new_node = malloc(sizeof(dfa_node_t));
    dfa_node_t *cur_node = NULL;
    for (int j = 0; !cur_node; ++j) {
      cur_node = partitions.data[i]->data[j];
    }
    vec_init(&new_node->next);
    new_node->bitset = bitset_copy(cur_node->bitset);
    vec_init(&new_node->chars);
    new_node->id = id;
    ++id;
    for (int j = 0; j < cur_node->next.length; ++j) {
      bool in_next = false;
      for (int k = 0; k < next->length; ++k) {
        if (cur_node->next.data[j]->partition == next->data[k]) {
          in_next = true;
          break;
        }
      }
      if (!in_next) {
        vec_push(&new_node->chars, bitset_copy(cur_node->chars.data[j]));
        vec_push(&new_node->next, NULL);
        vec_push(next, cur_node->next.data[j]->partition);
      }
    }
    vec_push(&pointers, next);
    vec_push(&result, new_node);
  }
  for (int i = 0; i < pointers.length; ++i) {
    for (int j = 0; j < pointers.data[i]->length; ++j) {
      result.data[i]->next.data[j] = result.data[pointers.data[i]->data[j]];
    }
  }
  return result;
}

static void emit_comment(const char *comment, ...) {
  va_list args;
  va_start(args, comment);
  while (comment) {
    printf("// %s\n", comment);
    comment = va_arg(args, const char *);
  }
  va_end(args);
}

static void emit_yy_next(const char *name) {
  emit_comment(
      "yy_next(state, c) is given the current state and next character,",
      "and evaluates to the next state.", NULL);
  printf("#define yy_next(state, c)    %s[state][c]\n", name);
}

static void emit_dfa_state_table(const dfa_t *dfa) {
  printf("{\n");
  printf("/* 00000 */ { ");
  for (char c = 0; c < 0x7F; ++c) {
    printf("   -1, ");
  }
  printf("},\n");
  for (int i = 0; i < dfa->length; ++i) {
    dfa_node_t *node = dfa->data[i];
    printf("/* %05d */ { ", i + 1);
    for (char c = 0; c < 0x7F; ++c) {
      bool found = false;
      for (int j = 0; j < node->next.length; ++j) {
        if (bitset_get(node->chars.data[j], c)) {
          found = true;
          printf("%5d, ", node->next.data[j]->index);
        }
      }
      if (!found) {
        printf("    0, ");
      }
    }
    printf("},\n");
  }
  printf("};\n");
}

#define NCOLS 10
#define TYPE "YY_TTYPE"
#define STORAGE_CLASS "YYPRIVATE"
#define DECODING_ROUTINE_STORAGE_CLASS "YYPRIVATE"
#define INDENT "          "

static void pairs(const dfa_t *dfa, const char *name, int threshold,
                  bool numbers) {
  int num_cells = 0;
  for (int i = 0; i < dfa->length; ++i) {
    int ntransitions = 0;
    for (int j = 0; j < dfa->data[i]->next.length; ++j) {
      ntransitions += bitset_count(dfa->data[i]->chars.data[j]);
    }
    if (ntransitions > 0) {
      printf("%s %s %s%-d[] = { ", STORAGE_CLASS, TYPE, name, i);
      ++num_cells;
      if (ntransitions > threshold) {
        printf("0,\n          ");
      } else {
        printf("%5d, ", ntransitions);
        if (threshold > 5) {
          printf("\n          ");
        }
      }
      int nprinted = NCOLS;
      int ncommas = ntransitions;
      for (int j = 0; j < 0x7F; ++j) {
        if (ntransitions > threshold) {
          ++num_cells;
          --nprinted;
        }
      }
    }
  }
}

int main(int argc, char *argv[]) {
  nfa_t nfa = thompson("^[ \\t]*//[ \\t]*TRACE[ \\t]*#[0-9]+[ \\t]*$");
  // nfa_print(&nfa);
  nfa_t nfa2 = thompson("^[ \\t]*#[0-9]+.*$");
  // nfa_print(&nfa2);

  dfa_t dfa = nfa_to_dfa(&nfa2);
  dfa_to_dot(&dfa);
  dfa_t min = minimize_dfa(&dfa);
  dfa_to_dot(&min);

  emit_yy_next("UNMIN_TABLE");
  printf("static const int UNMIN_TABLE[][] = ");
  emit_dfa_state_table(&dfa);

  nfa_free(&nfa);
  nfa_free(&nfa2);
  return 0;
}