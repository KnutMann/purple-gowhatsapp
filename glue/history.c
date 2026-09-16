#include "gowhatsapp.h"
#include "constants.h"
#include "libwhatsmeow.h"

static gulong conversation_created_signal = 0;
static int requests_suppressed = 0;

/*
 * Displaying a message brings its conversation into being if it did not exist,
 * which would otherwise read as "the user just opened this" and pull a heap of
 * old messages in underneath the new one. Requests are held during a display
 * for that reason, and it is the same guard that keeps the history we fetch
 * from asking for more history.
 */
void
gowhatsapp_history_suppress_requests(gboolean suppress) {
    if (suppress) {
        requests_suppressed++;
    } else if (requests_suppressed > 0) {
        requests_suppressed--;
    }
}

/*
 * A message the primary device sent back in answer to a history request.
 *
 * It travels the ordinary display path so it carries its message id like any
 * other message, but marked PURPLE_MESSAGE_DELAYED: a front-end can then show
 * it as the old news it is rather than announcing it. Its text arrives
 * pre-composed from the Go part and is escaped here like any other.
 */
void
gowhatsapp_display_history_message(gowhatsapp_message_t *gwamsg) {
    /* A media replay arrives as ready-made HTML - a link at a file the live
     * path already downloaded - and must not be escaped into visible tags. */
    gboolean escape = (gwamsg->subtype != gowhatsapp_text_subtype_history_html);
    gowhatsapp_display_text_message(
        gwamsg->account, gwamsg->senderJid, gwamsg->remoteJid, gwamsg->text,
        gwamsg->timestamp, gwamsg->isGroup, FALSE, gwamsg->name,
        PURPLE_MESSAGE_DELAYED, gwamsg->messageId, escape
    );
}

/*
 * The primary device has answered a request for one conversation's history.
 *
 * Fired even when the answer held nothing, which is the case worth reporting:
 * a front-end waiting for history can stop waiting instead of sitting out a
 * timeout for an answer that has already been and gone.
 */
void
gowhatsapp_handle_history_done(PurpleConnection *pc, gowhatsapp_message_t *gwamsg) {
    gchar *shown = g_strdup_printf("%d", (int)gwamsg->subtype);
    GHashTable *details = g_hash_table_new(g_str_hash, g_str_equal); // MEMCHECK: values are borrowed
    g_hash_table_insert(details, "chat", gwamsg->remoteJid);
    g_hash_table_insert(details, "shown", shown);
    g_hash_table_insert(details, "isGroup", gwamsg->isGroup ? "1" : "0");
    purple_signal_emit(purple_connection_get_prpl(pc), GOWHATSAPP_SIGNAL_HISTORY_DONE, pc, details);
    g_hash_table_destroy(details);
    g_free(shown);
}

/*
 * A conversation just came into being, either because the user opened it or
 * because a message arrived. Ask the primary device for what came before.
 *
 * The Go part holds off requests it has answered recently, so a conversation
 * being re-created (and the messages fetched here creating it again) does not
 * turn into a loop.
 */
static void conversation_created(PurpleConversation *conv, gpointer data) {
    PurpleAccount *account = purple_conversation_get_account(conv);
    const char *name = purple_conversation_get_name(conv);
    if (requests_suppressed) {
        // a message brought this conversation into being, not the user
        purple_debug_info("gowhatsapp", "not asking for history of %s: a message opened it\n", name);
        return;
    }
    if (!purple_strequal(purple_account_get_protocol_id(account), GOWHATSAPP_PRPL_ID)) {
        return;
    }
    int count = purple_account_get_int(account, GOWHATSAPP_FETCH_HISTORY_OPTION, 0);
    if (count <= 0) {
        purple_debug_info("gowhatsapp", "not asking for history of %s: switched off\n", name);
        return;
    }
    PurpleConnection *pc = purple_account_get_connection(account);
    WhatsappProtocolData *wpd = pc ? (WhatsappProtocolData *)purple_connection_get_protocol_data(pc) : NULL;
    if (wpd == NULL || wpd->history_requests_from == 0 || time(NULL) < wpd->history_requests_from) {
        // still settling in after connecting, where conversations appear by themselves
        purple_debug_info("gowhatsapp", "not asking for history of %s: still settling in\n", name);
        return;
    }
    char *who = (char *)purple_conversation_get_name(conv); // cgo does not suport const
    purple_debug_info("gowhatsapp", "asking for %d messages of history for %s\n", count, who);
    gowhatsapp_go_request_history(account, who, count); // this does all other checks (e.g. connection state)
}

/*
 * This registers a call-back: This prpl shall be notified every time a
 * conversation comes into being.
 */
void
gowhatsapp_history_init(PurpleConnection *pc) {
    if (!conversation_created_signal) {
        conversation_created_signal = purple_signal_connect(
            purple_conversations_get_handle(),
            "conversation-created",
            purple_connection_get_protocol(pc),
            PURPLE_CALLBACK(conversation_created),
            NULL
        );
    }
}
