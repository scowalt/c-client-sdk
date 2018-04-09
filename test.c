
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ldapi.h"

void
logger(const char *s)
{
    printf("LD says %s\n", s);
}

int
main(int argc, char **argv)
{
    printf("back to basics\n");

    LD_SetLogFunction(2, logger);

    LDConfig *config = LDConfigNew("authkey");

    LDUser *user = LDUserNew("user200");

    LDClient *client = LDClientInit(config, user);

    int delay = 0;
    while (true) {
    if (LDIntVariation(client, "bugcount", 10) > 5) {
        printf("it's greater than five\n");
    }
    if (LDBoolVariation(client, "sort.order", true)) {
        printf("sort order is true\n");
    } else {
        printf("sort order is false\n");
    }

    delay += 10;
    sleep(delay);
    
    printf("%d seconds up\n", delay);
    }

    return 0;
}

