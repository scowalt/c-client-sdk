
#include <stdbool.h>

#include "uthash.h"

typedef struct {
    char **strings;
} LDStringSet;

struct LDStringMap_i;

typedef enum {
    LDNodeNone = 0,
    LDNodeString,
    LDNodeNumber,
    LDNodeBool,
    LDNodeMap,
} LDNodeType;

typedef struct LDMapNode_i {
    char *key;
    LDNodeType type;
    union {
        bool b;
        char *s;
        double n;
        struct LDStringMap_s *m;
    };
    UT_hash_handle hh;
} LDMapNode;

typedef LDMapNode LDStringMap;

typedef struct LDConfig_i {
    bool allAttributesPrivate;
    int backgroundPollingIntervalMillis;
    char *appURI;
    int connectionTimeoutMillis;
    bool disableBackgroundUpdating;
    int eventsCapacity;
    int eventsFlushIntervalMillis;
    char *eventsURI;
    char *mobileKey;
    bool offline;
    int pollingIntervalMillis;
    LDStringSet *privateAttributeNames;
    bool streaming;
    char *streamURI;
    bool useReport;
} LDConfig;

typedef struct LDUser_i {
    char *key;
    bool anonymous;
    char *secondary;
    char *ip;
    char *firstName;
    char *lastName;
    char *email;
    char *name;
    char *avatar;
    LDStringMap *custom;
    LDStringSet *privateAttributeNames;
} LDUser;

struct LDClient_i;

typedef struct LDClient_i LDClient;

void LDSetString(char **, const char *);

LDConfig *LDConfigNew();
LDUser *LDUserNew(const char *);

LDClient *LDClientInit(LDConfig *, LDUser *);
LDClient *LDClientGet(void);

bool LDClientIsInitialized(LDClient *);
bool LDClientIsOffline(LDClient *);
void LDClientSetOffline(LDClient *);
void LDClientSetOnline(LDClient *);
void LDClientClose(LDClient *);

bool LDBoolVariation(LDClient *, const char *, bool);
int LDIntVariation(LDClient *, const char *, int);
double LDDoubleVariation(LDClient *, const char *, double);
char *LDStringVariationAlloc(LDClient *, const char *, const char *);
char *LDStringVariation(LDClient *, const char *, const char *, char *, size_t);

void LDFree(void *);

void LD_SetLogFunction(int userlevel, void (userlogfn)(const char *));

/* listener registration is unimplemented */


