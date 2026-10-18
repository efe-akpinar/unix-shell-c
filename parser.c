// parser.c
#include "parser.h"
#include <ctype.h>

// Extra parser node for cmd_args (one pipeline stage = one or more words).
// Allocated in initParser(), cleaned up in freeParser().
static mpc_parser_t *s_cmd_args_parser = NULL;

// ===================================================================================
// INTERNAL HELPERS
// ===================================================================================

typedef struct {
    char  **items;   // one entry per pipeline stage; each entry is a
                     // space-joined string that split_cmdvec() will argv-split
    size_t  len;
    size_t  cap;
} StrVec;

// Local strdup/strndup implementations for portability (avoid implicit
// declarations under strict C standards).
static char *xstrdup(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s) + 1;
    char  *p   = malloc(len);
    if (!p) return NULL;
    memcpy(p, s, len);
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    if (!s) s = "";
    size_t len = strlen(s);
    if (len > n) len = n;
    char *p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static void     free_cmdvec(CmdVec *R);
static void     free_cmdnode(CmdNode *node);
static CmdNode *build_cmd_tree(mpc_ast_t *node);
static void     print_cmdnode(const CmdNode *node, int depth);

// ===================================================================================
// VECTOR HELPERS
// ===================================================================================

static bool vec_push(StrVec *v, const char *s) {
    if (v->len == v->cap) {
        size_t  ncap   = v->cap ? v->cap * 2 : 8;
        char  **nitems = v->cap
            ? (char **)realloc(v->items, ncap * sizeof(char *))
            : (char **)malloc(ncap * sizeof(char *));
        if (!nitems) return false;
        v->items = nitems;
        v->cap   = ncap;
    }
    v->items[v->len] = xstrdup(s ? s : "");
    if (!v->items[v->len]) return false;
    v->len++;
    return true;
}

static void vec_free(StrVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; ++i)
        free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->len   = 0;
    v->cap   = 0;
}

// ===================================================================================
// CMD TOKEN SPLITTING
// ===================================================================================

static char **split_cmdvec(const char *s) {
    size_t  cap  = 8, argc = 0;
    char  **argv = malloc(cap * sizeof *argv);
    if (!argv) return NULL;

    const char *p = s;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '\'' || *p == '"') {
            quote = *p;
            p++;
        }

        const char *start = p;
        while (*p && (quote ? (*p != quote) : !isspace((unsigned char)*p)))
            p++;

        size_t len = (size_t)(p - start);
        char  *tok = xstrndup(start, len);
        if (!tok) goto fail;
        if (quote && *p == quote) p++;

        if (argc == cap) {
            cap *= 2;
            char **n = realloc(argv, cap * sizeof *n);
            if (!n) { free(tok); goto fail; }
            argv = n;
        }
        argv[argc++] = tok;
    }

    if (argc == cap) {
        char **n = realloc(argv, (cap + 1) * sizeof *n);
        if (!n) goto fail;
        argv = n;
    }
    argv[argc] = NULL;
    return argv;

fail:
    for (size_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return NULL;
}

static CmdVec strvec_to_cmdvec(const StrVec *v) {
    CmdVec R = {0};
    if (!v || v->len == 0) return R;
    R.n     = v->len;
    R.argvs = calloc(R.n, sizeof *R.argvs);
    if (!R.argvs) { R.n = 0; return R; }
    for (size_t i = 0; i < v->len; i++) {
        R.argvs[i] = split_cmdvec(v->items[i]);
        if (!R.argvs[i]) { free_cmdvec(&R); return (CmdVec){0}; }
    }
    return R;
}

static void free_cmdvec(CmdVec *R) {
    if (!R || !R->argvs) return;
    for (size_t i = 0; i < R->n; i++) {
        if (!R->argvs[i]) continue;
        for (size_t j = 0; R->argvs[i][j]; j++)
            free(R->argvs[i][j]);
        free(R->argvs[i]);
    }
    free(R->argvs);
    R->argvs = NULL;
    R->n     = 0;
}

// ===================================================================================
// AST HELPERS
// ===================================================================================

static bool ast_has_rule(const mpc_ast_t *node, const char *rule) {
    if (!node || !node->tag || !rule) return false;
    const char *tag = node->tag;
    size_t      len = strlen(rule);
    const char *p   = tag;

    while ((p = strstr(p, rule)) != NULL) {
        bool start_ok = (p == tag) || (p[-1] == '|');
        char next     = p[len];
        bool end_ok   = (next == '\0' || next == '|' || next == '>');
        if (start_ok && end_ok) return true;
        p += len;
    }
    return false;
}

static bool tag_contains(const mpc_ast_t *node, const char *s) {
    return node && node->tag && strstr(node->tag, s);
}

static mpc_ast_t *find_first_descendant(mpc_ast_t *node, const char *tagPart) {
    if (!node) return NULL;
    if (tag_contains(node, tagPart)) return node;
    for (int i = 0; i < node->children_num; i++) {
        mpc_ast_t *found = find_first_descendant(node->children[i], tagPart);
        if (found) return found;
    }
    return NULL;
}

static int count_direct_children_with_tag(mpc_ast_t *node, const char *tagPart) {
    int count = 0;
    if (!node) return 0;
    for (int i = 0; i < node->children_num; i++)
        if (tag_contains(node->children[i], tagPart)) count++;
    return count;
}

static void collect_direct_children_with_tag(mpc_ast_t *node, const char *tagPart,
                                              mpc_ast_t **out, int max_out) {
    int k = 0;
    if (!node) return;
    for (int i = 0; i < node->children_num && k < max_out; i++)
        if (tag_contains(node->children[i], tagPart))
            out[k++] = node->children[i];
}

// Rule-aware helpers that work even when mpca_lang collapses nodes and
// merges multiple rule names into a single tag string.
static mpc_ast_t *find_first_rule(mpc_ast_t *node, const char *rule) {
    if (!node) return NULL;
    if (ast_has_rule(node, rule)) return node;
    for (int i = 0; i < node->children_num; i++) {
        mpc_ast_t *found = find_first_rule(node->children[i], rule);
        if (found) return found;
    }
    return NULL;
}

static int count_direct_children_with_rule(mpc_ast_t *node, const char *rule) {
    int count = 0;
    if (!node) return 0;
    for (int i = 0; i < node->children_num; i++)
        if (ast_has_rule(node->children[i], rule)) count++;
    return count;
}

static void collect_direct_children_with_rule(mpc_ast_t *node, const char *rule,
                                              mpc_ast_t **out, int max_out) {
    int k = 0;
    if (!node) return;
    for (int i = 0; i < node->children_num && k < max_out; i++)
        if (ast_has_rule(node->children[i], rule))
            out[k++] = node->children[i];
}

// Join all cmdtok word descendants of a cmd_args subtree into one
// space-separated string. This works even when mpca_lang has collapsed
// the tree and merged tags such as "cmd_args|cmdtok|regex" into a
// single leaf node.
static void collect_words_into_str_rec(mpc_ast_t *node, char *buf, size_t bufsz) {
    if (!node) return;
    if (tag_contains(node, "cmdtok|regex")) {
        if (buf[0] != '\0')
            strncat(buf, " ", bufsz - strlen(buf) - 1);
        strncat(buf, node->contents, bufsz - strlen(buf) - 1);
    }
    for (int i = 0; i < node->children_num; i++)
        collect_words_into_str_rec(node->children[i], buf, bufsz);
}

static void collect_words_into_str(mpc_ast_t *node, char *buf, size_t bufsz) {
    buf[0] = '\0';
    collect_words_into_str_rec(node, buf, bufsz);
}

// Walk the pipe/cmd subtree.  Each cmd_args child = one pipeline stage.
// We push a space-joined word string per stage; split_cmdvec() will argv-split it.
static void collect_cmdtok_descendants(mpc_ast_t *node, StrVec *v) {
    if (!node) return;
    if (ast_has_rule(node, "cmd_args")) {
        char buf[4096];
        collect_words_into_str(node, buf, sizeof(buf));
        if (buf[0]) vec_push(v, buf);
        return;
    }
    for (int i = 0; i < node->children_num; i++)
        collect_cmdtok_descendants(node->children[i], v);
}

// ===================================================================================
// CMD TREE BUILDING
// ===================================================================================

static CmdNode *make_pipe_node_from_ast(mpc_ast_t *pipeNode) {
    StrVec   tmp  = {0};
    collect_cmdtok_descendants(pipeNode, &tmp);
    CmdNode *node = malloc(sizeof(CmdNode));
    if (!node) { vec_free(&tmp); return NULL; }
    node->type      = CMD_PIPE;
    node->data.pipe = strvec_to_cmdvec(&tmp);
    vec_free(&tmp);
    if (node->data.pipe.n == 0) { free(node); return NULL; }
    return node;
}

static CmdNode *build_cmd_tree(mpc_ast_t *node) {
    if (!node) return NULL;

    // First, check for the conditional form: exactly 3 direct Cmd children.
    int ncmd = count_direct_children_with_rule(node, "cmd");
    if (ncmd == 3) {
        mpc_ast_t *cmdChildren[3] = {0};
        collect_direct_children_with_rule(node, "cmd", cmdChildren, 3);
        CmdNode *root = malloc(sizeof(CmdNode));
        if (!root) return NULL;
        root->type                       = CMD_CONDITIONAL;
        root->data.conditional.cond      = build_cmd_tree(cmdChildren[0]);
        root->data.conditional.thenBranch = build_cmd_tree(cmdChildren[1]);
        root->data.conditional.elseBranch = build_cmd_tree(cmdChildren[2]);
        if (!root->data.conditional.cond ||
            !root->data.conditional.thenBranch ||
            !root->data.conditional.elseBranch) {
            free_cmdnode(root);
            return NULL;
        }
        return root;
    }

    // Otherwise, if this subtree contains a direct child Pipe, treat it as a pipe expression.
    for (int i = 0; i < node->children_num; i++) {
        if (ast_has_rule(node->children[i], "pipe")) {
            return make_pipe_node_from_ast(node->children[i]);
        }
    }

    // Last fallback: maybe this node itself has a pipe somewhere below.
    mpc_ast_t *pipeNode = find_first_rule(node, "pipe");
    if (pipeNode) return make_pipe_node_from_ast(pipeNode);

    return NULL;
}

static void free_cmdnode(CmdNode *node) {
    if (!node) return;
    if (node->type == CMD_PIPE) {
        free_cmdvec(&node->data.pipe);
    } else {
        free_cmdnode(node->data.conditional.cond);
        free_cmdnode(node->data.conditional.thenBranch);
        free_cmdnode(node->data.conditional.elseBranch);
    }
    free(node);
}

// ===================================================================================
// REDIRECTION PARSING
// ===================================================================================

static void parse_redirection(mpc_ast_t *redirNode, compiledCmd *cmd) {
    if (!redirNode || !cmd) return;
    /* Scan this node's direct children for ">" or "<" followed by a
     * filename, then recurse into children so nested optin/optout
     * nodes are also handled. */
    for (int i = 0; i < redirNode->children_num; i++) {
        mpc_ast_t *ch = redirNode->children[i];
        if (tag_contains(ch, "char")) {
            if (strcmp(ch->contents, ">") == 0) {
                for (int j = i + 1; j < redirNode->children_num; j++) {
                    if (tag_contains(redirNode->children[j], "fname|regex")) {
                        if (!cmd->outFile)
                            cmd->outFile = xstrdup(redirNode->children[j]->contents);
                        break;
                    }
                }
            } else if (strcmp(ch->contents, "<") == 0) {
                for (int j = i + 1; j < redirNode->children_num; j++) {
                    if (tag_contains(redirNode->children[j], "fname|regex")) {
                        if (!cmd->inFile)
                            cmd->inFile = xstrdup(redirNode->children[j]->contents);
                        break;
                    }
                }
            }
        }
    }
    for (int i = 0; i < redirNode->children_num; i++)
        parse_redirection(redirNode->children[i], cmd);
}

// ===================================================================================
// PARSER INIT / FREE
// ===================================================================================

int initParser(sparser_t *p) {
    p->Fname    = mpc_new("fname");
    p->CmdTok   = mpc_new("cmdtok");
    p->Pipe     = mpc_new("pipe");
    p->Cmd      = mpc_new("cmd");
    p->OptIn    = mpc_new("optin");
    p->OptOut   = mpc_new("optout");
    p->Redir    = mpc_new("redir");
    p->OptRedir = mpc_new("optredir");
    p->Quit     = mpc_new("quit");
    p->Line     = mpc_new("line");

    // Grammar design:
    // MPCA_LANG_DEFAULT skips all whitespace, so cmdtok can only match one word.
    // We add cmd_args to group multiple words into one pipeline stage:
    //   cmdtok   : single word, no spaces, no shell meta-chars
    //   cmd_args : one or more cmdtok words (= one pipeline stage)
    //   pipe     : cmd_args separated by pipe chars
    //   cmd      : conditional (cmd ? cmd : cmd) or plain pipe
    //   redir    : optional I/O redirection at end of line
    //   line     : quit | cmd optredir
    // CmdArgs is allocated here and stored in s_cmd_args_parser for cleanup.

    mpc_parser_t *CmdArgs = mpc_new("cmd_args");

    /* Grammar:
     *
     *   Line     → Quit | Cmd OptRedir
     *   Quit     → "exit"
     *   Cmd      → "(" Cmd "?" Cmd ":" Cmd ")" | Pipe
     *   Pipe     → CMD PipeTail
     *   PipeTail → "|" CMD PipeTail | ϵ
     *   OptRedir → Redir | ϵ
     *   Redir    → ">" FNAME OptIn | "<" FNAME OptOut
     *   OptIn    → "<" FNAME | ϵ
     *   OptOut   → ">" FNAME | ϵ
     *
     * Here the terminal `CMD` is represented by one or more `cmdtok`
     * words grouped into a `cmd_args` node; this is later split into
     * argv arrays when building the `CmdVec`.
     */
    mpc_err_t *e = mpca_lang(MPCA_LANG_DEFAULT,
        " fname    : /[A-Za-z_][A-Za-z0-9_.]*/        ;                  \n"
        " cmdtok   : /[-0-9A-Za-z,.\"']+/             ;                  \n"
        " cmd_args : <cmdtok>+                        ;                  \n"
        " pipe     : <cmd_args> ('|' <cmd_args>)*     ;                  \n"
        " cmd      : '(' <cmd> '?' <cmd> ':' <cmd> ')' | <pipe> ;        \n"
        " optin    : ('<' <fname>)?                   ;                  \n"
        " optout   : ('>' <fname>)?                   ;                  \n"
        " redir    : '>' <fname> <optin> | '<' <fname> <optout> ;        \n"
        " optredir : <redir>?                         ;                  \n"
        " quit     : \"exit\"                         ;                  \n"
        " line     : <quit> | <cmd> <optredir>        ;                  \n",
        p->Fname, p->CmdTok, CmdArgs, p->Pipe, p->Cmd,
        p->OptIn, p->OptOut, p->Redir, p->OptRedir, p->Quit, p->Line, NULL);

    if (e) {
        mpc_err_print(e);
        mpc_err_delete(e);
        mpc_delete(CmdArgs);
        return 0;
    }
    /* Store CmdArgs so freeParser can clean it up.
     * We reuse the OptIn slot temporarily -- no: just store in a static.
     * Simplest: store pointer in the unused padding of sparser_t by casting.
     * Actually cleanest: stash it in a file-scope variable. */
    s_cmd_args_parser = CmdArgs;
    return 1;
}

void freeParser(sparser_t *p) {
    mpc_cleanup(11,
        p->Fname, p->CmdTok, s_cmd_args_parser, p->Pipe, p->Cmd,
        p->OptIn, p->OptOut, p->Redir, p->OptRedir, p->Quit, p->Line);
    s_cmd_args_parser = NULL;
}

// ===================================================================================
// COMPILED COMMAND MANAGEMENT
// ===================================================================================

void initCompiledCmd(compiledCmd *cmd) {
    if (!cmd) return;
    cmd->root    = NULL;
    cmd->inFile  = NULL;
    cmd->outFile = NULL;
    cmd->isQuit  = 0;
}

void freeCompiledCmd(compiledCmd *cmd) {
    if (!cmd) return;
    free_cmdnode(cmd->root);  cmd->root    = NULL;
    free(cmd->inFile);        cmd->inFile  = NULL;
    free(cmd->outFile);       cmd->outFile = NULL;
    cmd->isQuit = 0;
}

// ===================================================================================
// MAIN COMPILE FUNCTION
// ===================================================================================

int compileCommand(sparser_t *sp, char *input, compiledCmd *cmd) {
    mpc_result_t result;
    initCompiledCmd(cmd);

    if (!mpc_parse("<stdin>", input, sp->Line, &result)) {
        mpc_err_print(result.error);
        mpc_err_delete(result.error);
        return 0;
    }

    mpc_ast_t *ast = result.output;
    /* Debug helper available: mpc_ast_print(ast); */

    if (ast_has_rule(ast, "quit") || find_first_rule(ast, "quit")) {
        cmd->isQuit = 1;
        mpc_ast_delete(ast);
        return 1;
    }
    mpc_ast_t *cmdAst = find_first_rule(ast, "cmd");
    if (!cmdAst) {
        mpc_ast_delete(ast);
        return 0;
    }

    cmd->root = build_cmd_tree(cmdAst);
    if (!cmd->root) {
        mpc_ast_delete(ast);
        freeCompiledCmd(cmd);
        return 0;
    }

    mpc_ast_t *redirAst = find_first_rule(ast, "redir");
    if (redirAst) parse_redirection(redirAst, cmd);

    mpc_ast_delete(ast);
    return 1;
}

// ===================================================================================
// DEBUG PRINTING
// ===================================================================================

static void indent(int d) {
    for (int i = 0; i < d; i++) printf("  ");
}

static void print_cmdvec_internal(const CmdVec *cv, int depth) {
    indent(depth);
    printf("PIPE (%zu commands)\n", cv->n);
    for (size_t i = 0; i < cv->n; i++) {
        indent(depth + 1);
        printf("[%zu] ", i);
        if (cv->argvs[i])
            for (size_t j = 0; cv->argvs[i][j]; j++)
                printf("'%s' ", cv->argvs[i][j]);
        printf("\n");
    }
}

static void print_cmdnode(const CmdNode *node, int depth) {
    if (!node) { indent(depth); printf("(null)\n"); return; }
    if (node->type == CMD_PIPE) {
        indent(depth); printf("CMD_PIPE\n");
        print_cmdvec_internal(&node->data.pipe, depth + 1);
    } else {
        indent(depth); printf("CMD_CONDITIONAL\n");
        indent(depth + 1); printf("COND:\n");
        print_cmdnode(node->data.conditional.cond, depth + 2);
        indent(depth + 1); printf("THEN:\n");
        print_cmdnode(node->data.conditional.thenBranch, depth + 2);
        indent(depth + 1); printf("ELSE:\n");
        print_cmdnode(node->data.conditional.elseBranch, depth + 2);
    }
}

void printCompiledCmd(compiledCmd *c) {
    if (!c) { printf("Command is NULL.\n"); return; }
    printf("--- Parsed Command ---\n");
    if (c->isQuit) {
        printf("Quit command detected: exit\n");
        printf("----------------------\n");
        return;
    }
    print_cmdnode(c->root, 0);
    if (c->inFile)  printf("Input  redirection: %s\n", c->inFile);
    else            printf("No input  redirection.\n");
    if (c->outFile) printf("Output redirection: %s\n", c->outFile);
    else            printf("No output redirection.\n");
    printf("----------------------\n");
}