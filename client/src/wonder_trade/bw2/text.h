#ifndef WONDER_TRADE_BW2_TEXT_H
#define WONDER_TRADE_BW2_TEXT_H

/* Wonder Trade's English text for Black 2 and White 2, from the Generation
 * IV messages (client/files/msgdata/msg/msg_0829.json); \n starts a new line.
 * The compiler reads source bytes, so wide literals hold ASCII only and write
 * other characters as escapes (\x00e9 for e acute). The apostrophe is ASCII,
 * as in the game's own English text. */
enum WtText {
    WT_TEXT_TITLE,
    WT_TEXT_TRADE,
    WT_TEXT_INFO,
    WT_TEXT_EXIT,
    WT_TEXT_RETRY,
    WT_TEXT_BACK,
    WT_TEXT_CANCEL,
    WT_TEXT_CONNECTING,
    WT_TEXT_DISCONNECTING,
    WT_TEXT_CHOOSE,
    WT_TEXT_PINGING,
    WT_TEXT_CONNECTION_ERROR,
    WT_TEXT_PING_ERROR,
    WT_TEXT_INELIGIBLE,
    WT_TEXT_DUPLICATE,
    WT_TEXT_EXPLANATION,
    WT_TEXT_CHECKING_SAVE,
    WT_TEXT_RECOVERY_REQUIRED,
    WT_TEXT_CONFIRM,
    WT_TEXT_QUEUED,
    WT_TEXT_COLLECT,
    WT_TEXT_CANCEL_OFFER,
    WT_TEXT_CONFIRM_COLLECT,
    WT_TEXT_CONFIRM_CANCEL,
    WT_TEXT_COMPLETED,
    WT_TEXT_RECEIVED,
    WT_TEXT_ORIGINAL_TRAINER,
    WT_TEXT_RETURNED,
    WT_TEXT_REJECTED,
    WT_TEXT_SUPERSEDED,
    WT_TEXT_WAITING,
    WT_TEXT_NO_ROOM,
    WT_TEXT_KEEP_PLAYING,
    WT_TEXT_CONFIRM_ACTION,
    WT_TEXT_RECOVERY_ACTION,
    WT_TEXT_CANNOT_HOLD,
    WT_TEXT_LEVEL,
    WT_TEXT_REFUSED_EGG,
    WT_TEXT_REFUSED_MAIL,
    WT_TEXT_REFUSED_GAME,
    WT_TEXT_REFUSED_ILLEGAL,
    WT_TEXT_REFUSED_NAME,
    WT_TEXT_ERROR_CODE,
    WT_TEXT_PICK,
    WT_TEXT_COUNT,
};

extern const unsigned short *const gWtText[WT_TEXT_COUNT];

#endif
