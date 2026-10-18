#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "parser.h"

static int solve_node(CmdNode *curr,int fromFd,int toFd);
static int solve_pipe(CmdVec *lineSet,int fromFd,int toFd);

static void safe_close(int fd) {
    if (fd >= 0 && fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        close(fd);
    }
}

static int line_is_empty(const char *txt) {
    while (*txt) {
        if (*txt != ' ' && *txt != '\t' && *txt != '\r') {
            return 0;
        }
        txt++;
    }
    return 1;
}

static void exec_side(char **words,int leftFd,int rightFd,int errFd,int keep1,int keep2) {
    if (leftFd != STDIN_FILENO) {
        if (dup2(leftFd,STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(1);
        }
    }

    if (rightFd != STDOUT_FILENO) {
        if (dup2(rightFd,STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(1);
        }
    }

    if (errFd != STDERR_FILENO) {
        if (dup2(errFd,STDERR_FILENO) < 0) {
            perror("dup2");
            _exit(1);
        }
    }

    safe_close(leftFd);
    safe_close(rightFd);
    safe_close(errFd);
    safe_close(keep1);
    safe_close(keep2);

    if (!words || !words[0]) {
        _exit(0);
    }

    execvp(words[0],words);
    perror(words[0]);
    _exit(127);
}

static int solve_pipe(CmdVec *lineSet,int fromFd,int toFd) {
    if (!lineSet || lineSet->n == 0) {
        return 0;
    }

    pid_t *childList = malloc(lineSet->n * sizeof(pid_t));
    if (!childList) {
        perror("malloc");
        return 1;
    }

    int errLine[2];
    if (pipe(errLine) < 0) {
        perror("pipe");
        free(childList);
        return 1;
    }

    int lastRead = -1;

    for (size_t pos = 0; pos < lineSet->n; pos++) {
        int midLine[2] = {-1,-1};

        if (pos + 1 < lineSet->n) {
            if (pipe(midLine) < 0) {
                perror("pipe");
                if (lastRead >= 0) close(lastRead);
                close(errLine[0]);
                close(errLine[1]);
                for (size_t done = 0; done < pos; done++) {
                    waitpid(childList[done],NULL,0);
                }
                free(childList);
                return 1;
            }
        }

        pid_t kid = fork();
        if (kid < 0) {
            perror("fork");
            if (lastRead >= 0) close(lastRead);
            if (midLine[0] >= 0) close(midLine[0]);
            if (midLine[1] >= 0) close(midLine[1]);
            close(errLine[0]);
            close(errLine[1]);
            for (size_t done = 0; done < pos; done++) {
                waitpid(childList[done],NULL,0);
            }
            free(childList);
            return 1;
        }

        if (kid == 0) {
            int useIn = (lastRead >= 0) ? lastRead : fromFd;
            int useOut = (pos + 1 < lineSet->n) ? midLine[1] : toFd;

            if (midLine[0] >= 0) close(midLine[0]);
            close(errLine[0]);

            exec_side(lineSet->argvs[pos],useIn,useOut,errLine[1],fromFd,toFd);
        }

        childList[pos] = kid;

        if (lastRead >= 0) {
            close(lastRead);
        }
        if (midLine[1] >= 0) {
            close(midLine[1]);
        }

        lastRead = midLine[0];
    }

    if (lastRead >= 0) {
        close(lastRead);
    }
    close(errLine[1]);

    int seenErr = 0;
    char tempBuf[4096];
    ssize_t gotSize;

    while ((gotSize = read(errLine[0],tempBuf,sizeof(tempBuf))) > 0) {
        seenErr = 1;

        ssize_t moved = 0;
        while (moved < gotSize) {
            ssize_t once = write(STDERR_FILENO,tempBuf + moved,(size_t)(gotSize - moved));
            if (once < 0) {
                break;
            }
            moved += once;
        }
    }

    close(errLine[0]);

    for (size_t pos = 0; pos < lineSet->n; pos++) {
        waitpid(childList[pos],NULL,0);
    }

    free(childList);
    return seenErr;
}

static int solve_node(CmdNode *curr,int fromFd,int toFd) {
    if (!curr) {
        return 0;
    }

    if (curr->type == CMD_PIPE) {
        return solve_pipe(&curr->data.pipe,fromFd,toFd);
    }

    int condErr = solve_node(curr->data.conditional.cond,fromFd,STDOUT_FILENO);

    if (condErr) {
        int altErr = solve_node(curr->data.conditional.elseBranch,STDIN_FILENO,toFd);
        return condErr || altErr;
    }

    int okErr = solve_node(curr->data.conditional.thenBranch,STDIN_FILENO,toFd);
    return condErr || okErr;
}

int main(void) {
    sparser_t sp;

    if (!initParser(&sp)) {
        return 1;
    }

    char *raw = NULL;
    size_t rawCap = 0;

    while (1) {
        printf("shell$ ");
        fflush(stdout);

        ssize_t count = getline(&raw,&rawCap,stdin);
        if (count < 0) {
            break;
        }

        if (count > 0 && raw[count - 1] == '\n') {
            raw[count - 1] = '\0';
        }

        if (line_is_empty(raw)) {
            continue;
        }

        compiledCmd pack;
        if (!compileCommand(&sp,raw,&pack)) {
            fprintf(stderr,"parse error\n");
            freeCompiledCmd(&pack);
            continue;
        }

        if (pack.isQuit) {
            freeCompiledCmd(&pack);
            printf("Exiting shell...\n");
            break;
        }

        int enterFd = STDIN_FILENO;
        int leaveFd = STDOUT_FILENO;
        int tookIn = 0;
        int tookOut = 0;
        int badOpen = 0;

        if (pack.inFile) {
            enterFd = open(pack.inFile,O_RDONLY);
            if (enterFd < 0) {
                perror(pack.inFile);
                badOpen = 1;
            } else {
                tookIn = 1;
            }
        }

        if (!badOpen && pack.outFile) {
            leaveFd = open(pack.outFile,O_WRONLY | O_CREAT | O_TRUNC,0644);
            if (leaveFd < 0) {
                perror(pack.outFile);
                badOpen = 1;
            } else {
                tookOut = 1;
            }
        }

        if (!badOpen) {
            solve_node(pack.root,enterFd,leaveFd);
        }

        if (tookIn) {
            close(enterFd);
        }

        if (tookOut) {
            close(leaveFd);
        }

        freeCompiledCmd(&pack);
    }

    free(raw);
    freeParser(&sp);
    return 0;
}
