#define LEVEL           "qnvme"

#define LOG_NORM(msg)   \
    printf("%s: %s\n", LEVEL, msg);
#define LOG_ERR(msg)    \
    printf(stderr, "%s-ERR:__FILE__:__LINE__: %s\n", LEVEL, msg);
#ifdef DEBUG
#define LOG_DBG(msg)    \
    fprintf(stderr, "%s-DBG:__FILE__:__LINE__: %s\n", LEVEL, msg);
#else
#define LOG_DBG(msg)    ;
#endif
