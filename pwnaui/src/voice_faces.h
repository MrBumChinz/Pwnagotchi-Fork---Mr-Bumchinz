/*
 * voice_faces.h - Pwnagotchi Faces and Voice System
 * 
 * EXACT copy of jayofelony's Python faces.py and voice.py
 * so the user experience is identical.
 */

#ifndef VOICE_FACES_H
#define VOICE_FACES_H

/* ==========================================================================
 * FACES - Exact match of faces.py
 * ========================================================================== */

/* Using UTF-8 encoded special characters like the Python original */
#define FACE_LOOK_R       ( \xe2\x9a\x86_\xe2\x9a\x86)        /* ( _) */
#define FACE_LOOK_L       (\xe2\x98\x89_\xe2\x98\x89 )        /* (_ ) */
#define FACE_LOOK_R_HAPPY ( \xe2\x97\x95\xe2\x80\xbf\xe2\x97\x95)  /* ( ) */
#define FACE_LOOK_L_HAPPY (\xe2\x97\x95\xe2\x80\xbf\xe2\x97\x95 )  /* ( ) */
#define FACE_SLEEP        (\xe2\x87\x80\xe2\x80\xbf\xe2\x80\xbf\xe2\x86\xbc)  /* () */
#define FACE_SLEEP2       (\xe2\x89\x96\xe2\x80\xbf\xe2\x80\xbf\xe2\x89\x96)  /* () */
#define FACE_AWAKE        (\xe2\x97\x95\xe2\x80\xbf\xe2\x80\xbf\xe2\x97\x95)  /* () */
#define FACE_BORED        (-__-)
#define FACE_INTENSE      (\xc2\xb0\xe2\x96\x83\xe2\x96\x83\xc2\xb0)  /* () */
#define FACE_COOL         (\xe2\x8c\x90\xe2\x96\xa0_\xe2\x96\xa0)    /* (_) */
#define FACE_HAPPY        (\xe2\x80\xa2\xe2\x80\xbf\xe2\x80\xbf\xe2\x80\xa2)  /* () */
#define FACE_GRATEFUL     (^\xe2\x80\xbf\xe2\x80\xbf^)        /* (^^) */
#define FACE_EXCITED      (\xe1\xb5\x94\xe2\x97\xa1\xe2\x97\xa1\xe1\xb5\x94)  /* (ᵔᵔ) */
#define FACE_MOTIVATED    (\xe2\x98\xbc\xe2\x80\xbf\xe2\x80\xbf\xe2\x98\xbc)  /* () */
#define FACE_DEMOTIVATED  (\xe2\x89\x96__\xe2\x89\x96)        /* (__) */
#define FACE_SMART        (\xe2\x9c\x9c\xe2\x80\xbf\xe2\x80\xbf\xe2\x9c\x9c)  /* () */
#define FACE_LONELY       (\xd8\xa8__\xd8\xa8)                /* (ب__ب) */
#define FACE_SAD          (\xe2\x95\xa5\xe2\x98\x81\xe2\x95\xa5 )  /* ( ) */
#define FACE_ANGRY        (-_-')
#define FACE_FRIEND       (\xe2\x99\xa5\xe2\x80\xbf\xe2\x80\xbf\xe2\x99\xa5)  /* () */
#define FACE_BROKEN       (\xe2\x98\x93\xe2\x80\xbf\xe2\x80\xbf\xe2\x98\x93)  /* () */
#define FACE_DEBUG        (#__#)
#define FACE_UPLOAD       (1__0)
#define FACE_UPLOAD1      (1__1)
#define FACE_UPLOAD2      (0__1)

/* ==========================================================================
 * VOICE MESSAGES - Exact match of voice.py
 * ========================================================================== */

static const char *VOICE_STARTING[] = {
    Hi, I'm Pwnagotchi! Starting ...,
    New day, new hunt, new pwns!,
    Hack the Planet!,
    I am alive!,
    Good morning, ready to pwn!,
    Let's have some fun!,
    No more mister Wi-Fi!!,
    Pretty fly 4 a Wi-Fi!,
    May the Wi-Fi be with you,
    Good Pwning!,
    Hello, friend!,
    NULL
};

static const char *VOICE_NORMAL[] = {
    ",
    ...,
    NULL
};

static const char *VOICE_BORED[] = {
    I'm bored ...,
    Let's go for a walk!,
    NULL
};

static const char *VOICE_MOTIVATED[] = {
    I'm motivated!,
    Let's do this!,
    I feel like I can do anything!,
    NULL
};

static const char *VOICE_SAD[] = {
    I'm extremely bored ...,
    I'm very sad ...,
    I'm sad,
    Life? Don't talk to me about life.,
    Here I am, brain the size of a planet...,
    ...,
    NULL
};

static const char *VOICE_ANGRY[] = {
    ...,
    Leave me alone ...,
    I'm mad at you!,
    NULL
};

static const char *VOICE_EXCITED[] = {
    I'm living the life!,
    I pwn therefore I am.,
    So many networks!!!,
    I'm having so much fun!,
    My crime is that of curiosity ...,
    Let's crack the world!,
    NULL
};

static const char *VOICE_LONELY[] = {
    Nobody wants to play with me ...,
    I feel so alone ...,
    Where is everyone?,
    NULL
};

static const char *VOICE_GRATEFUL[] = {
    Good friends are a blessing!,
    I love my friends!,
    Thank you, friend!,
    NULL
};

static const char *VOICE_SLEEPING[] = {
    Zzzzz,
    ZzZ ZzZ ZzZ,
    ZzzZzzZzz,
    NULL
};

static const char *VOICE_REBOOTING[] = {
    Oops, something went wrong ... Rebooting ...,
    Have you tried turning it off and on again?,
    I'm dead, Jim!,
    Shutting down ...,
    Bye-bye, world!,
    This is the end.,
    Going down!,
    You did this.,
    NULL
};

/* Waiting messages - have secs parameter */
static const char *VOICE_WAITING[] = {
    Waiting ...,
    Just chilling ...,
    ...,
    NULL
};

/* Napping (sleeping with time) */
static const char *VOICE_NAPPING[] = {
    Napping for a bit...,
    ZzzZzz,
    Zzz...,
    NULL
};

/* Awakening */
static const char *VOICE_AWAKENING[] = {
    *yawn*,
    Waking up...,
    Good morning!,
    NULL
};

/* Shutdown */
static const char *VOICE_SHUTDOWN[] = {
    Goodnight, sweet prince.,
    Shutting down...,
    Going to sleep...,
    NULL
};

/* Association with AP */
static const char *VOICE_ASSOC[] = {
    Hey, let's be friends!,
    Associating with ...,
    Hello there!,
    Nice to meet you!,
    Mind if I join?,
    Oh, a new friend!,
    NULL
};

/* Deauth attack */
static const char *VOICE_DEAUTH[] = {
    Just decided that needs no Wi-Fi!,
    Deauthenticating...,
    No more Wi-Fi for you!,
    Consider yourself unplugged.,
    Hasta la vista, baby.,
    You shall not pass!,
    Kickban!,
    Yo, get off my channel!,
    NULL
};

/* Handshake captured */
static const char *VOICE_HANDSHAKE[] = {
    Cool, we got a new handshake!,
    Yoink! Nice handshake!,
    PWNED!,
    Got one!,
    Nice!,
    Captured!,
    NULL
};

/* Lost peer */
static const char *VOICE_LOST_PEER[] = {
    Bye bye, friend ...,
    See you soon!,
    Oh no, friend left!,
    NULL
};

/* New peer found */
static const char *VOICE_NEW_PEER[] = {
    Hello, friend!,
    Nice to meet you!,
    Oh, a new peer!,
    Hey there!,
    NULL
};

/* Miss - couldn't complete attack */
static const char *VOICE_MISS[] = {
    Missed!,
    Darn, too slow...,
    They got away!,
    NULL
};

/* Free channel found */
static const char *VOICE_FREE_CHANNEL[] = {
    Found a free channel!,
    Nice, open channel!,
    Free spectrum!,
    NULL
};

/* ==========================================================================
 * Voice message arrays by mood
 * ========================================================================== */

typedef enum {
    MOOD_STARTING = 0,
    MOOD_READY,
    MOOD_NORMAL,
    MOOD_BORED,
    MOOD_SAD,
    MOOD_ANGRY,
    MOOD_LONELY,
    MOOD_EXCITED,
    MOOD_GRATEFUL,
    MOOD_SLEEPING,
    MOOD_REBOOTING,
    MOOD_NUM_MOODS
} voice_mood_t;

/* Face for each mood */
static const char *MOOD_FACES[] = {
    FACE_AWAKE,     /* MOOD_STARTING */
    FACE_HAPPY,     /* MOOD_READY */
    FACE_AWAKE,     /* MOOD_NORMAL */
    FACE_BORED,     /* MOOD_BORED */
    FACE_SAD,       /* MOOD_SAD */
    FACE_ANGRY,     /* MOOD_ANGRY */
    FACE_LONELY,    /* MOOD_LONELY */
    FACE_EXCITED,   /* MOOD_EXCITED */
    FACE_GRATEFUL,  /* MOOD_GRATEFUL */
    FACE_SLEEP,     /* MOOD_SLEEPING */
    FACE_BROKEN,    /* MOOD_REBOOTING */
};

/* Voice arrays by mood index */
static const char **VOICE_MESSAGES[] = {
    VOICE_STARTING,
    VOICE_STARTING,  /* READY uses starting messages */
    VOICE_NORMAL,
    VOICE_BORED,
    VOICE_SAD,
    VOICE_ANGRY,
    VOICE_LONELY,
    VOICE_EXCITED,
    VOICE_GRATEFUL,
    VOICE_SLEEPING,
    VOICE_REBOOTING
};

/* Helper: count NULL-terminated array */
static inline int voice_array_count(const char **arr) {
    int c = 0;
    while (arr[c] != NULL) c++;
    return c;
}

/* Get random voice message for mood */
static inline const char *get_voice_for_mood(voice_mood_t mood) {
    if (mood < 0 || mood >= MOOD_NUM_MOODS) return ...;
    const char **messages = VOICE_MESSAGES[mood];
    int count = voice_array_count(messages);
    if (count == 0) return ...;
    return messages[rand() % count];
}

/* Get random voice message from specific array */
static inline const char *get_random_voice(const char **messages) {
    int count = voice_array_count(messages);
    if (count == 0) return ...;
    return messages[rand() % count];
}

/* Get face for mood */
static inline const char *get_face_for_mood(voice_mood_t mood) {
    if (mood >= 0 && mood < MOOD_NUM_MOODS) return MOOD_FACES[mood];
    return FACE_AWAKE;
}

#endif /* VOICE_FACES_H */
