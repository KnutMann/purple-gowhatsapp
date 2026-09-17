#include "gowhatsapp.h"
#include "libwhatsmeow.h"

static int
send_message(PurpleConnection *pc, const gchar *who, const gchar *message, gboolean is_group) {
    /* A front-end that lets the user rewrite something already sent leaves the id of that
     * message on the conversation, and this is where it is spent. Taken out before anything
     * else can return early: left lying around it would attach itself to whatever is typed
     * next, and the wrong message would be replaced. Allocated by the front-end, freed here. */
    PurpleConversation *conv = is_group
        ? purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, who, purple_connection_get_account(pc))
        : purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who, purple_connection_get_account(pc));
    char *edit_target = conv ? purple_conversation_get_data(conv, GOWHATSAPP_EDIT_TARGET_KEY) : NULL;
    if (conv) {
        purple_conversation_set_data(conv, GOWHATSAPP_EDIT_TARGET_KEY, NULL);
    }

    char *msg = NULL;
    if (purple_account_get_bool(purple_connection_get_account(pc), GOWHATSAPP_BRIDGE_COMPATIBILITY_OPTION, FALSE)) {
        // Bridge Mode: Spectrum allegedly does not do HTML and bitlbee is probably plain-text anyways, so use message as it is, preserving new-lines
        // see https://github.com/hoehermann/purple-gowhatsapp/issues/257
        msg = g_strdup(message);
    } else {
        // Strip HTML similar to these reasons: https://github.com/majn/telegram-purple/issues/12 and https://github.com/majn/telegram-purple/commit/fffe7519d7269cf4e5029a65086897c77f5283ac
        // Note: This turns newlines into spaces and <br> tags into newlines
        msg = purple_markup_strip_html(message);
    }
    PurpleAccount *account = purple_connection_get_account(pc);
    char *w = (char *)who; // cgo does not suport const
    int ret = edit_target
        ? gowhatsapp_go_send_edit(account, w, msg, is_group, edit_target)
        : gowhatsapp_go_send_message(account, w, msg, is_group);
    g_free(edit_target);
    g_free(msg);
    return ret;
}

int
gowhatsapp_send_im(PurpleConnection *pc, const gchar *who, const gchar *message, PurpleMessageFlags flags) {
    if (is_command(message)) {
        return execute_command(pc, message, who, NULL);
    } else {
        return send_message(pc, who, message, FALSE);
    }
}

int
gowhatsapp_send_chat(
    PurpleConnection *pc, int id, const gchar *message, PurpleMessageFlags flags
) {
    PurpleConversation *conv = purple_find_chat(pc, id);
    if (conv != NULL) {
        gchar *who = (gchar *)purple_conversation_get_data(conv, "name");
        if (who != NULL) {
            if (is_command(message)) {
                return execute_command(pc, message, who, conv);
            } else {
                int ret = send_message(pc, who, message, TRUE);
                if (ret > 0) {
                    // Group chats need an explicit local echo since the implicit echo is implemented for direct messages only.
                    // See https://keep.imfreedom.org/pidgin/pidgin/file/v2.14.12/libpurple/conversation.c#l191.
                    PurpleConvChat *conv_chat = purple_conversation_get_chat_data(conv);
                    PurpleAccount *account = purple_conversation_get_account(conv);
                    purple_conv_chat_write(conv_chat, purple_account_get_username(account), message, flags, time(NULL));
                }
                return ret;
            }
        }
    }
    return -6; // a negative value to indicate failure. chose ENXIO "no such address"
}
