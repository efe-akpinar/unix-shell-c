#ifndef SPARSER_H
#define SPARSER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "mpc.h"

// Shell grammar:
//   Line     → Quit | Cmd OptRedir
//   Quit     → "exit"
//   Cmd      → "(" Cmd "?" Cmd ":" Cmd ")" | Pipe
//   Pipe     → CMD PipeTail
//   PipeTail → "|" CMD PipeTail | ϵ
//   OptRedir → Redir | ϵ
//   Redir    → ">" FNAME OptIn | "<" FNAME OptOut
//   OptIn    → "<" FNAME | ϵ
//   OptOut   → ">" FNAME | ϵ
//
// Here CMD is represented in the AST as one `cmd_args` node, which
// groups one or more `cmdtok` words that later get split into argv[].

// ===================================================================================
// PUBLIC DATA STRUCTURES
// ===================================================================================

// A pipeline such as:
//   ls -l | wc -l
typedef struct {
    char ***argvs;   // array of argv arrays
    size_t n;        // number of commands in the pipeline
} CmdVec;

// Cmd -> "(" Cmd "?" Cmd ":" Cmd ")" | Pipe
typedef enum {
    CMD_PIPE,
    CMD_CONDITIONAL
} CmdType;

typedef struct CmdNode CmdNode;
struct CmdNode {
    CmdType type;
    union {
        CmdVec pipe;
        struct {
            CmdNode *cond;
            CmdNode *thenBranch;
            CmdNode *elseBranch;
        } conditional;
    } data;
};

// Final parsed line
typedef struct {
    CmdNode *root;   // full parsed command tree
    char *inFile;    // input redirection filename or NULL
    char *outFile;   // output redirection filename or NULL
    int isQuit;      // 1 if "exit", 0 otherwise
} compiledCmd;

typedef struct {
    mpc_parser_t *Fname;
    mpc_parser_t *CmdTok;
    mpc_parser_t *Pipe;
    mpc_parser_t *Cmd;
    mpc_parser_t *OptIn;
    mpc_parser_t *OptOut;
    mpc_parser_t *Redir;
    mpc_parser_t *OptRedir;
    mpc_parser_t *Quit;
    mpc_parser_t *Line;
} sparser_t;

// ===================================================================================
// PUBLIC FUNCTIONS
// ===================================================================================

int  initParser(sparser_t *p);
void freeParser(sparser_t *p);
int  compileCommand(sparser_t *sp, char *input, compiledCmd *cmd);
void initCompiledCmd(compiledCmd *cmd);
void freeCompiledCmd(compiledCmd *cmd);
void printCompiledCmd(compiledCmd *c);

#endif