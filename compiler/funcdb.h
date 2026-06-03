#ifndef FUNCDB_H_
#define FUNCDB_H_

#include "enumdb.h"
#include <stdio.h>

#define FUNCDB_MAX_FUNCS    512
#define FUNCDB_MAX_PARAMS    16
#define FUNCDB_MAX_FORWARDS 128
#define FUNCDB_CALL_DEPTH    16
#define FUNCDB_NAME_LEN      80

/* Records that parameter `my_param` of this function is forwarded as
   argument `callee_arg` to function `callee`. */
typedef struct {
    char callee[FUNCDB_NAME_LEN];
    int  callee_arg;
    int  my_param;
} ParamForward;

typedef struct {
    char         name[FUNCDB_NAME_LEN];
    int          n_params;
    char         params[FUNCDB_MAX_PARAMS][FUNCDB_NAME_LEN];
    int          n_forwards;
    ParamForward forwards[FUNCDB_MAX_FORWARDS];
} FuncDef;

typedef struct {
    FuncDef defs[FUNCDB_MAX_FUNCS];
    int     n_defs;
} FuncDB;

/* Pre-scan source to collect user-function definitions and param-forwarding.
   May be called once per script file; accumulates into db. */
void funcdb_prescan(FuncDB *db, const char *src);

/* Propagate enum-type annotations from enumdb through param-forwarding triples.
   Iterates to fixed point. Returns total new annotations added. */
int funcdb_propagate(FuncDB *db, EnumDB *enumdb);

/* Print inferred annotations (those added after enumdb->n_annots_file) to f.
   Output format matches enum-annotations.txt. */
void funcdb_print_annots(const EnumDB *enumdb, FILE *f);

#endif /* FUNCDB_H_ */
