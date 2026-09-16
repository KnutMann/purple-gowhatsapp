#include "purple_compat.h"
#include "bridge.h"

#define GOWHATSAPP_NAME "whatsmeow"  // name to refer to this plug-in (in logs)
#define GOWHATSAPP_PRPL_ID "prpl-hehoe-whatsmeow"

/*
 * Signal emitted on the protocol plug-in (find it via purple_find_prpl or
 * purple_connection_get_prpl) whenever a receipt comes in: a message was sent
 * by this instance, delivered to the remote end, read there, or read by
 * another device of this account. Arguments: the PurpleConnection and a
 * GHashTable of borrowed strings with the keys
 * chat, sender, id, type (a gowhatsapp_receipt_type name), isGroup ("0"/"1"),
 * timestamp (seconds since the epoch, as decimal string).
 */
#define GOWHATSAPP_SIGNAL_RECEIPT "gowhatsapp-receipt"

/*
 * Two more signals with the same calling convention.
 *
 * GOWHATSAPP_SIGNAL_MESSAGE_ID fires right before a message is written into a
 * conversation, so consumers can associate the line that is about to appear
 * with the protocol-level id later reactions refer to. Keys:
 * chat, sender, id, isGroup ("0"/"1"), isOutgoing ("0"/"1"),
 * timestamp (seconds since the epoch, as decimal string).
 *
 * GOWHATSAPP_SIGNAL_REACTION fires when a reaction comes in: sender put emoji
 * onto the message named by id, or took it back (empty emoji). Keys:
 * chat, sender, id, emoji, isGroup ("0"/"1"), timestamp.
 */
#define GOWHATSAPP_SIGNAL_MESSAGE_ID "gowhatsapp-message-id"
#define GOWHATSAPP_SIGNAL_REACTION "gowhatsapp-reaction"

/*
 * GOWHATSAPP_SIGNAL_HISTORY_DONE fires when the primary device has answered a
 * request for a conversation's earlier messages, including when it answered
 * with none. Keys: chat, shown (how many were displayed, as a number), isGroup.
 *
 * It exists because the answer is worth knowing about even when it is empty.
 * The phone hands over each stretch of history once; ask a second time and the
 * answer is a legitimate nothing. A front-end that holds a window back while it
 * waits for fetched history has otherwise no way to tell that from an answer
 * still on its way, and can only sit out a timeout for something that has
 * already arrived and was empty.
 */
#define GOWHATSAPP_SIGNAL_HISTORY_DONE "gowhatsapp-history-done"

#define GOWHATSAPP_STATUS_STR_AVAILABLE "available" // this must match whatsmeow's types.PresenceAvailable
#define GOWHATSAPP_STATUS_STR_AWAY      "unavailable" // this must match whatsmeow's types.PresenceUnavailable
#define GOWHATSAPP_STATUS_STR_OFFLINE   "offline"
#define GOWHATSAPP_STATUS_STR_MOBILE    "mobile"

// protocol data for one connection
typedef struct {
    // reference to roomlist which is currently being populated in asynchronous calls
    PurpleRoomlist *roomlist;
    // when this connection was established
    time_t connected_at_timestamp;
    // not before this may a new conversation ask the phone for history, see history.c
    time_t history_requests_from;
} WhatsappProtocolData;

// options
GList *gowhatsapp_add_account_options(GList *account_options);

// login
void gowhatsapp_login(PurpleAccount *account);
void gowhatsapp_close(PurpleConnection *pc);
void gowhatsapp_store_credentials(PurpleAccount *account, char *credentials);

// qrcode
void gowhatsapp_handle_qrcode(PurpleConnection *pc, gowhatsapp_message_t *gwamsg);
void gowhatsapp_close_qrcode(PurpleAccount *account);

// process_message
void gowhatsapp_process_message(gowhatsapp_message_t *gwamsg);

// display_message
void gowhatsapp_display_text_message(PurpleAccount *account, const gchar * senderJid, const gchar * remoteJid, const gchar * text, const time_t timestamp, const gboolean isGroup, const gboolean isOutgoing, const gchar * name, PurpleMessageFlags flags, const gchar * messageId, const gboolean escape);

// message_filtering
gboolean gowhatsapp_append_message_id_if_not_exists(PurpleAccount *account, char *message_id);
gboolean gowhatsapp_message_is_new_enough(PurpleAccount *account, const time_t ts);

// groups
PurpleConversation *gowhatsapp_enter_group_chat(PurpleConnection *pc, const char *remoteJid, char **participants);
void gowhatsapp_join_chat(PurpleConnection *pc, GHashTable *data);
char *gowhatsapp_get_chat_name(GHashTable *components);
PurpleRoomlist *gowhatsapp_roomlist_get_list(PurpleConnection *pc);
void gowhatsapp_set_chat_topic(PurpleConnection *pc, int id, const char *topic);
gchar *gowhatsapp_roomlist_serialize(PurpleRoomlistRoom *room);
GList * gowhatsapp_chat_info(PurpleConnection *pc);
GHashTable * gowhatsapp_chat_info_defaults(PurpleConnection *pc, const char *chat_name);
void gowhatsapp_chat_set_participants(PurpleConvChat *conv_chat, char **participants);
void gowhatsapp_roomlist_add_room(PurpleConnection *pc, char *remoteJid, char *name);
void gowhatsapp_handle_group(PurpleConnection *pc, gowhatsapp_message_t *gwamsg);
void gowhatsapp_free_name(PurpleConversation *conv);
char *gowhatsapp_get_cb_alias(PurpleConnection *gc, int id, const char *who);

// blist
PurpleBuddy * gowhatsapp_ensure_buddy_in_blist(PurpleAccount *account, const char *remoteJid, const char *display_name, gboolean authoritative);
PurpleChat * gowhatsapp_ensure_group_chat_in_blist(PurpleAccount *account, const char *remoteJid, const char *topic);
PurpleGroup * gowhatsapp_get_purple_group();
PurpleChat * gowhatsapp_find_blist_chat(PurpleAccount *account, const char *jid);
void gowhatsapp_add_buddy(PurpleConnection *pc, PurpleBuddy *buddy, PurpleGroup *group);
void gowhatsapp_tooltip_text(PurpleBuddy *buddy, PurpleNotifyUserInfo *info, gboolean full);
void gowhatsapp_assume_buddy_away(PurpleAccount *account, PurpleBuddy *buddy);
void gowhatsapp_for_all_buddies(PurpleAccount *account, void(*func)(PurpleAccount *, PurpleBuddy *));
void gowhatsapp_for_all_chats(PurpleAccount *account, void(*func)(PurpleAccount *, PurpleChat *));
void gowhatsapp_request_chat_profile_picture(PurpleAccount *account, PurpleChat *chat);

// send_message
int gowhatsapp_send_im(PurpleConnection *pc, const gchar *who, const gchar *message, PurpleMessageFlags flags);
unsigned int gowhatsapp_send_typing(PurpleConnection *pc, const gchar *who, PurpleTypingState state);
int gowhatsapp_send_chat(PurpleConnection *pc, int id, const gchar *message, PurpleMessageFlags flags);

// handle_attachment
void gowhatsapp_handle_attachment(gowhatsapp_message_t *gwamsg);

/* Stamps the system quarantine attribute on a file the plug-in downloaded, so
 * Gatekeeper interposes on a later open, as it would for a browser download.
 * Implemented in quarantine.m; a no-op on error, which is only ever logged. */
void gowhatsapp_quarantine_downloaded_file(const char *local_path);
char * gowhatsapp_attachment_fill_template(const char *template, time_t timestamp, const char *hash, const char *filename, const char *extension, const char *remote, const char *sender, const char *title, const char *alias, const char *messageid, PurpleMessageFlags flags);

// send_file
PurpleXfer * gowhatsapp_new_xfer(PurpleConnection *pc, const char *who);
void gowhatsapp_send_file(PurpleConnection *pc, const gchar *who, const gchar *filename);
void gowhatsapp_chat_send_file(PurpleConnection *pc, int id, const char *filename);

// presence
void gowhatsapp_handle_presence(PurpleAccount *account, char *remoteJid, char available, time_t last_seen);
void gowhatsapp_set_presence(PurpleAccount *account, PurpleStatus *status);
void gowhatsapp_subscribe_presence_updates(PurpleAccount *account, PurpleBuddy *buddy);

// profile pictures
void gowhatsapp_request_profile_picture(PurpleAccount *account, PurpleBuddy *buddy);
void gowhatsapp_handle_profile_picture(gowhatsapp_message_t *gwamsg);

// receipts
void gowhatsapp_receipts_init(PurpleConnection *pc);
void gowhatsapp_handle_receipt(PurpleConnection *pc, gowhatsapp_message_t *gwamsg);

// reactions
void gowhatsapp_handle_reaction(PurpleConnection *pc, gowhatsapp_message_t *gwamsg);

// history fetched from the primary device
void gowhatsapp_history_init(PurpleConnection *pc);
void gowhatsapp_display_history_message(gowhatsapp_message_t *gwamsg);
void gowhatsapp_history_suppress_requests(gboolean suppress);
void gowhatsapp_handle_history_done(PurpleConnection *pc, gowhatsapp_message_t *gwamsg);

// commands
enum gowhatsapp_command {
    GOWHATSAPP_COMMAND_NONE = 0,
    GOWHATSAPP_COMMAND_VERSIONS,
    GOWHATSAPP_COMMAND_CONTACTS,
    GOWHATSAPP_COMMAND_PARTICIPANTS,
    GOWHATSAPP_COMMAND_PRESENCE,
    GOWHATSAPP_COMMAND_LOGOUT
};
enum gowhatsapp_command is_command(const char *message);
int execute_command(PurpleConnection *pc, const gchar *message, const gchar *who, PurpleConversation *conv);
