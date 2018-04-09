#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>

#include "curl/curl.h"

#include "ldapi.h"
#include "ldinternal.h"


static pthread_once_t clientonce = PTHREAD_ONCE_INIT;
static pthread_t eventthread;
static pthread_t pollingthread;
static pthread_t streamingthread;

static LDClient *theClient;
static pthread_rwlock_t clientlock = PTHREAD_RWLOCK_INITIALIZER;

void
LDSetString(char **target, const char *value)
{
    free(*target);
    if (value) {
        *target = strdup(value);
    } else {
        *target = NULL;
    }
}

LDConfig *
LDConfigNew(const char *mobileKey)
{
    LDConfig *config;

    config = malloc(sizeof(*config));
    config->allAttributesPrivate = false;
    config->backgroundPollingIntervalMillis = 3600000;
    LDSetString(&config->appURI, "https://app.launchdarkly.com");
    config->connectionTimeoutMillis = 10000;
    config->disableBackgroundUpdating = false;
    config->eventsCapacity = 100;
    config->eventsFlushIntervalMillis = 30000;
    LDSetString(&config->eventsURI, "https://mobile.launchdarkly.com");
    LDSetString(&config->mobileKey, mobileKey);
    config->offline = false;
    config->pollingIntervalMillis = 300000;
    config->privateAttributeNames = NULL;
    config->streaming = true;
    LDSetString(&config->streamURI, "https://clientstream.launchdarkly.com");
    config->useReport = false;

    return config;
}

LDUser *
LDUserNew(const char *key)
{
    LDUser *user;

    user = malloc(sizeof(*user));
    LDSetString(&user->key, key);
    user->anonymous = false;
    user->secondary = NULL;
    user->ip = NULL;
    user->firstName = NULL;
    user->lastName = NULL;
    user->email = NULL;
    user->name = NULL;
    user->avatar = NULL;
    user->custom = NULL;
    user->privateAttributeNames = NULL;

    return user;
}

static void
milliSleep(int ms)
{
    sleep(ms / 1000);
}

char *
LDi_usertourl(LDUser *user)
{
    cJSON *jsonuser = LDi_usertojson(user);
    char *textuser = cJSON_Print(jsonuser);
    cJSON_Delete(jsonuser);
    size_t b64len;
    char *b64text = LDi_base64_encode(textuser, strlen(textuser), &b64len);
    free(textuser);
    return b64text;
}


static void *
bgeventsender(void *v)
{
    LDClient *client = v;

    while (true) {
        LDi_rdlock(&clientlock);
        int ms = client->config->eventsFlushIntervalMillis;
        LDi_unlock(&clientlock);

        LDi_log(20, "bg sender sleeping\n");
        milliSleep(ms);
        LDi_log(20, "bgsender running\n");

        char *eventdata = LDi_geteventdata();
        if (!eventdata) {
            LDi_log(20, "no event data to send\n");
            continue;
        }

        LDi_rdlock(&clientlock);
        if (client->dead) {
            LDi_unlock(&clientlock);
            continue;
        }
        
        bool sent = false;
        int retries = 0;
        while (!sent) {
            char url[4096];
            snprintf(url, sizeof(url), "%s/mobile", client->config->eventsURI);
            char authkey[256];
            snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey);
            LDi_unlock(&clientlock);
            /* unlocked while sending; will relock if retry needed */
            int response = 0;
            LDi_sendevents(url, authkey, eventdata, &response);
            if (response == 401 || response == 403) {
                sent = true; /* consider it done */
                client->dead = true;
                retries = 0;
            } else if (response == -1) {
                retries++;
            } else {
                sent = true;
                retries = 0;
            }
            if (retries) {
                int backoff = 1000 * pow(2, retries - 2);
                backoff += random() % backoff;
                if (backoff > 3600 * 1000) {
                    backoff = 3600 * 1000;
                    retries--; /* avoid excessive incrementing */
                }
                milliSleep(backoff);
                LDi_rdlock(&clientlock);
            }
        }
        free(eventdata);
    }
}

/*
 * this thread always runs, even when using streaming, but then it just sleeps
 */
static void *
bgfeaturepoller(void *v)
{
    LDClient *client = v;
    char *prevdata = NULL;

    while (true) {
        LDi_rdlock(&clientlock);
        int ms = client->config->pollingIntervalMillis;
        bool skippolling = client->config->streaming;
        LDi_unlock(&clientlock);

        LDi_log(20, "bg poller sleeping\n");
        milliSleep(ms);
        if (skippolling) {
            continue;
        }
        LDi_log(20, "bg poller running\n");

        LDi_rdlock(&clientlock);
        if (client->dead) {
            LDi_unlock(&clientlock);
            continue;
        }
        
        char *userurl = LDi_usertourl(client->user);
        char url[4096];
        snprintf(url, sizeof(url), "%s/msdk/eval/users/%s", client->config->appURI, userurl);
        free(userurl);
        char authkey[256];
        snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey);

        LDi_unlock(&clientlock);
        
        int response = 0;
        LDMapNode *hash = LDi_fetchfeaturemap(url, authkey, &response);
        if (response == 401 || response == 403) {
            client->dead = true;
        }
        if (!hash)
            continue;
        LDMapNode *oldhash;

        LDi_wrlock(&clientlock);
        oldhash = client->allFlags;
        client->allFlags = hash;
        LDi_unlock(&clientlock);
        LDi_freehash(oldhash);
    }
}

static void
onstreameventput(const char *data)
{
    cJSON *payload = cJSON_Parse(data);

    if (!payload) {
        LDi_log(5, "parsing failed\n");
        return;
    }
    LDMapNode *hash = NULL;
    if (payload->type == cJSON_Object) {
        hash = LDi_jsontohash(payload, 1);
    }
    cJSON_Delete(payload);

    LDi_wrlock(&clientlock);
    LDMapNode *oldhash = theClient->allFlags;
    theClient->allFlags = hash;
    LDi_unlock(&clientlock);

    LDi_freehash(oldhash);
}

static void
onstreameventpatch(const char *data)
{
    cJSON *payload = cJSON_Parse(data);

    if (!payload) {
        LDi_log(5, "parsing patch failed\n");
        return;
    }
    LDMapNode *patch = NULL;
    if (payload->type == cJSON_Object) {
        patch = LDi_jsontohash(payload, 2);
    }
    cJSON_Delete(payload);

    LDi_wrlock(&clientlock);
    LDMapNode *hash = theClient->allFlags;
    LDMapNode *node, *tmp;
    HASH_ITER(hh, patch, node, tmp) {
        LDMapNode *res = NULL;
        HASH_FIND_STR(hash, node->key, res);
        if (res) {
            HASH_DEL(hash, res);
            LDi_freenode(res);
        }
        HASH_DEL(patch, node);
        HASH_ADD_KEYPTR(hh, hash, node->key, strlen(node->key), node);
    }

    theClient->allFlags = hash;
    LDi_unlock(&clientlock);

    LDi_freehash(patch);

}

static void
onstreameventping(void)
{
    LDClient *client = theClient;

    LDi_rdlock(&clientlock);
    char *userurl = LDi_usertourl(client->user);
    char url[4096];
    snprintf(url, sizeof(url), "%s/msdk/eval/users/%s", client->config->appURI, userurl);
    free(userurl);
    char authkey[256];
    snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey);

    LDi_unlock(&clientlock);

    int response = 0;
    LDMapNode *hash = LDi_fetchfeaturemap(url, authkey, &response);
    if (response == 401 || response == 403) {
        client->dead = true;
    }
    if (!hash)
        return;
    LDMapNode *oldhash;

    LDi_wrlock(&clientlock);
    oldhash = client->allFlags;
    client->allFlags = hash;
    LDi_unlock(&clientlock);
    LDi_freehash(oldhash);
}

/*
 * as far as event stream parsers go, this is pretty basic.
 * assumes that there's only one line of data following an event identifier line.
 * : -> comment gets eaten
 * event:type -> type is remembered for the next line
 * data:line -> line is processed according to last seen event type
 */
static int
streamcallback(const char *line)
{
    static int wantnewevent = 1;
    static char eventtypebuf[256];

    if (*line == ':') {
        LDi_log(10, "i reject your comment\n");
    } else if (wantnewevent) {
        LDi_log(15, "this better be a new event...\n");
        char *eventtype = strchr(line, ':');
        if (!eventtype || eventtype[1] == 0) {
            LDi_log(5, "unsure\n");
            return 1;
        }
        snprintf(eventtypebuf, sizeof(eventtypebuf), "%s", eventtype + 1);
        wantnewevent = 0;
    } else if (*line == 0) {
        LDi_log(15, "end of event\n");
        wantnewevent = 1;
    } else {
        if (strncmp(line, "data:", 5) != 0) {
            LDi_log(5, "not data\n");
            return 1;
        }
        line += 5;
        if (strcmp(eventtypebuf, "put") == 0) {
            LDi_log(15, "PUT\n");
            onstreameventput(line);
        } else if (strcmp(eventtypebuf, "patch") == 0) {
            LDi_log(15, "PATCH\n");
            onstreameventpatch(line);
        } else if (strcmp(eventtypebuf, "ping") == 0) {
            LDi_log(15, "PING\n");
            onstreameventping();
        }
#if 0
        printf("here is data for the event %s\n", eventtypebuf);
        printf("the data: %s\n", line);
#endif
    }
    return 0;
}

static void *
bgfeaturestreamer(void *v)
{
    LDClient *client = v;

    while (true) {
        LDi_rdlock(&clientlock);
        char *userurl = LDi_usertourl(client->user);

        char url[4096];
        snprintf(url, sizeof(url), "%s/meval/%s", client->config->streamURI, userurl);
        free(userurl);
        
        char authkey[256];
        snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey);
        LDi_unlock(&clientlock);

        int response;
        /* this won't return until it disconnects */
        LDi_readstream(url, authkey, &response, streamcallback);

        /* need some better backoff logic here */
        milliSleep(30000);
    }
}

static void
starteverything(void)
{

    curl_global_init(CURL_GLOBAL_DEFAULT);

    theClient = malloc(sizeof(*theClient));
    memset(theClient, 0, sizeof(*theClient));

    pthread_create(&eventthread, NULL, bgeventsender, theClient);
    pthread_create(&pollingthread, NULL, bgfeaturepoller, theClient);
    pthread_create(&streamingthread, NULL, bgfeaturestreamer, theClient);
}

static void
checkconfig(LDConfig *config)
{


}

LDClient *
LDClientInit(LDConfig *config, LDUser *user)
{
    checkconfig(config);

    LDi_initevents(config->eventsCapacity);

    LDi_wrlock(&clientlock);

    pthread_once(&clientonce, starteverything);

    theClient->config = config;
    theClient->user = user;
    theClient->dead = false;

    int response;
    LDMapNode *hash = NULL;
    theClient->allFlags = hash;

    LDi_unlock(&clientlock);

    LDi_recordidentify(user);

    LDi_log(10, "init done\n");
    return theClient;
}

LDClient *
LDClientGet()
{
    return theClient;
}

static LDMapNode *
lookupnode(LDMapNode *hash, const char *key)
{
    LDMapNode *res = NULL;

    LDMapNode *node, *tmp;

    HASH_FIND_STR(hash, key, res);   
    return res;
}

bool
LDBoolVariation(LDClient *client, const char *key, bool fallback)
{
    LDMapNode *res;
    bool b;

    LDi_rdlock(&clientlock);
    res = lookupnode(client->allFlags, key);
    if (res && res->type == LDNodeBool) {
        LDi_log(15, "found result\n");    
        b = res->b;
    } else {
        LDi_log(15, "no result for %s\n", key);
        b = fallback;
    }
    LDi_unlock(&clientlock);
    LDi_recordfeature(client->user, key, LDNodeBool, (double)b, NULL, (double)fallback, NULL);
    return b;
}

int
LDIntVariation(LDClient *client, const char *key, int fallback)
{
    LDMapNode *res;
    int i;

    LDi_rdlock(&clientlock);
    res = lookupnode(client->allFlags, key);
    if (res && res->type == LDNodeNumber)
        i = (int)res->n;
    else
        i = fallback;
    LDi_unlock(&clientlock);
    LDi_recordfeature(client->user, key, LDNodeNumber, (double)i, NULL, (double)fallback, NULL);
    return i;
}

double
LDDoubleVariation(LDClient *client, const char *key, double fallback)
{
    LDMapNode *res;
    double d;

    LDi_rdlock(&clientlock);
    res = lookupnode(client->allFlags, key);
    if (res && res->type == LDNodeNumber)
        d = res->n;
    else
        d = fallback;
    LDi_unlock(&clientlock);
    LDi_recordfeature(client->user, key, LDNodeNumber, d, NULL, fallback, NULL);
    return d;
}

char *
LDStringVariation(LDClient *client, const char *key, const char *fallback,
    char *buffer, size_t space)
{
    LDMapNode *res;
    const char *s;
    size_t len;

    LDi_rdlock(&clientlock);
    res = lookupnode(client->allFlags, key);
    if (res && res->type == LDNodeString)
        s = res->s;
    else
        s = fallback;
    
    len = strlen(s);
    if (len > space - 1)
        len = space - 1;
    memcpy(buffer, s, len);
    buffer[len] = 0;
    LDi_unlock(&clientlock);
    LDi_recordfeature(client->user, key, LDNodeString, 0.0, buffer, 0.0, fallback);
    return buffer;
}

char *
LDStringVariationAlloc(LDClient *client, const char *key, const char *fallback)
{
    LDMapNode *res;
    const char *s;
    char *news;

    LDi_rdlock(&clientlock);
    res = lookupnode(client->allFlags, key);
    if (res && res->type == LDNodeString)
        s = res->s;
    else
        s = fallback;
    
    news = strdup(s);
    LDi_unlock(&clientlock);
    LDi_recordfeature(client->user, key, LDNodeString, 0.0, news, 0.0, fallback);
    return news;
}
