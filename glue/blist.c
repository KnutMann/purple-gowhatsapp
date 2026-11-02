#include <execinfo.h>

#include "gowhatsapp.h"
#include "constants.h"
#include "libwhatsmeow.h" // for gowhatsapp_go_subscribe_presence

PurpleGroup * gowhatsapp_get_purple_group() {
    PurpleGroup *group = purple_blist_find_group("Whatsapp");
    if (!group) {
        group = purple_group_new("Whatsapp"); // MEMCHECK: caller takes ownership
        purple_blist_add_group(group, NULL);
    }
    return group;
}

void gowhatsapp_assume_buddy_away(PurpleAccount *account, PurpleBuddy *buddy) {
    g_return_if_fail(buddy != NULL);

    if (purple_account_get_bool(account, GOWHATSAPP_FAKE_ONLINE_OPTION, TRUE)) {
        purple_prpl_got_user_status(account, buddy->name, GOWHATSAPP_STATUS_STR_AWAY, NULL);
        purple_prpl_got_user_status(account, buddy->name, GOWHATSAPP_STATUS_STR_MOBILE, NULL);
    }
}

/*
 * Hands a buddy back the name the server last gave it.
 *
 * libpurple keeps a buddy's server alias in memory only. blist.xml saves the
 * local alias, and this plug-in leaves that one to the user. So after a restart
 * every buddy is nameless until the contact sync speaks again, and for a buddy
 * whose name has not changed the sync says nothing: gowhatsapp_ensure_buddy_in_blist
 * compares against the name it recorded. libpurple then seats such a buddy in a
 * group chat under the bare JID, and does not even ask get_cb_alias, because a
 * buddy was found. The recorded name goes back to the buddy list directly, not
 * through purple_serv_got_alias, which would write "is now known as" into every
 * open conversation at every login.
 */
void gowhatsapp_restore_server_alias(PurpleAccount *account, PurpleBuddy *buddy) {
    (void)account;
    g_return_if_fail(buddy != NULL);
    const char *recorded = purple_blist_node_get_string(&buddy->node, "server_alias");
    if (recorded != NULL && *recorded && !purple_strequal(purple_buddy_get_server_alias(buddy), recorded)) {
        purple_blist_server_alias_buddy(buddy, recorded);
    }
}

/*
 * A contact can end up in the list twice: this plug-in files every contact
 * under its own "Whatsapp" group, and a front-end adding the same contact to
 * a group the user chose may create a second node instead of moving ours
 * (issue #215 observes the result). Two nodes for one JID split presence,
 * icon and alias bookkeeping between them. The moment the same JID exists
 * outside the automatic group, the automatic node has been superseded:
 * remove it and keep the user's. Removal is local only, which is all there
 * is, WhatsApp has no server-side roster.
 */
static PurpleBuddy * gowhatsapp_supersede_automatic_group_node(PurpleAccount *account, const char *identifier) {
    /* purple_find_buddies with an empty name returns every buddy of the
     * account; without this guard an empty identifier would read as "all of
     * the automatic group has been superseded" and delete it wholesale. */
    if (identifier == NULL || identifier[0] == 0) {
        return NULL;
    }
    PurpleGroup *automatic_group = purple_find_group("Whatsapp");
    if (automatic_group == NULL) {
        return NULL;
    }
    PurpleBuddy *keep = NULL;
    GSList *automatic_nodes = NULL;
    GSList *buddies = purple_find_buddies(account, identifier);
    while (buddies != NULL) {
        PurpleBuddy *buddy = buddies->data;
        if (purple_buddy_get_group(buddy) == automatic_group) {
            automatic_nodes = g_slist_prepend(automatic_nodes, buddy);
        } else if (keep == NULL) {
            keep = buddy;
        }
        buddies = g_slist_delete_link(buddies, buddies);
    }
    while (automatic_nodes != NULL) {
        if (keep != NULL) {
            purple_blist_remove_buddy(automatic_nodes->data);
        }
        automatic_nodes = g_slist_delete_link(automatic_nodes, automatic_nodes);
    }
    return keep;
}

/*
 * Ensure buddy in the buddy list.
 * Updates alias non-destructively.
 *
 * identifier is the username (purple who)
 * name is the human readable name (purple alias).
 */
PurpleBuddy * gowhatsapp_ensure_buddy_in_blist(PurpleAccount *account, const char *identifier, const char *name, gboolean authoritative) {
    if (purple_str_has_suffix(identifier, "@lid")) {
        // TODO: combine into existing non-hidden buddy
        return NULL;
    }

    /* Groups are chats (bookmarks), not contacts: never create a buddy
     * for a group JID. Opening such a pseudo-contact would start a 1:1
     * conversation with the group JID, which silently goes nowhere.
     *
     * The guard is loud on purpose. It was added from the symptom, and the
     * caller that reaches it was never identified; upstream cannot find one
     * either. So whenever it does fire it names itself, with the stack that
     * led here, and the question is answered by a log line instead of by
     * another guess. */
    if (purple_str_has_suffix(identifier, "@g.us")) {
        void *frames[12];
        int depth = backtrace(frames, G_N_ELEMENTS(frames));
        char **names = backtrace_symbols(frames, depth);
        purple_debug_warning(GOWHATSAPP_NAME, "GROUP JID AS BUDDY: %s (name %s). Refused. Stack:\n", identifier, name ? name : "(none)");
        for (int i = 1; i < depth; i++) {
            purple_debug_warning(GOWHATSAPP_NAME, "    %s\n", names ? names[i] : "?");
        }
        free(names); // MEMCHECK: backtrace_symbols allocates the array with malloc
        return NULL;
    }

    PurpleBuddy *buddy = gowhatsapp_supersede_automatic_group_node(account, identifier);
    if (buddy == NULL) {
        buddy = purple_blist_find_buddy(account, identifier);
    }

    if (!buddy) {
        PurpleGroup *group = gowhatsapp_get_purple_group();
        buddy = purple_buddy_new(account, identifier, NULL); // MEMCHECK: blist takes ownership; no local alias – that belongs to the user
        purple_blist_add_buddy(buddy, NULL, group, NULL);
        gowhatsapp_subscribe_presence_updates(account, buddy);
    }

    /* Update the server-side alias only. The local alias stays under the user's
     * control. Authoritative names come from the contact sync (the address book
     * name); non-authoritative ones are push names riding along with messages,
     * which must never overwrite a synced name. */
    if (name != NULL && *name) {
        const char *server_alias = purple_blist_node_get_string(&buddy->node, "server_alias");
        gboolean update = authoritative ? !purple_strequal(server_alias, name)
                                        : (server_alias == NULL || !*server_alias);
        if (update) {
            purple_serv_got_alias(purple_account_get_connection(account), identifier, name);
            purple_blist_node_set_string(&buddy->node, "server_alias", name); // persist so there is no name-change reported after a restart
        }
    }

    return buddy;
}

/*
 * This is called after a buddy has been added to the buddy list 
 * (i.e. by manual user interaction).
 */
void gowhatsapp_add_buddy(PurpleConnection *pc, PurpleBuddy *buddy, PurpleGroup *group) {
    PurpleAccount *account = purple_connection_get_account(pc);
    gowhatsapp_assume_buddy_away(account, buddy);
    gowhatsapp_subscribe_presence_updates(account, buddy);
}

/*
 * Calls a function once on each buddy.
 */
void gowhatsapp_for_all_buddies(PurpleAccount *account, void(*func)(PurpleAccount *, PurpleBuddy *)) {
    g_return_if_fail(account != NULL);
    GSList *buddies = purple_find_buddies(account, NULL);
    while (buddies != NULL) {
        func(account, buddies->data);
        buddies = g_slist_delete_link(buddies, buddies);
    }
}

/*
 * Calls a function once on each group chat of this account.
 *
 * The counterpart of gowhatsapp_for_all_buddies, and needed because a group is deliberately not a
 * buddy here: a chat cannot be messaged like a contact, and a contact list row for one is a row that
 * leads nowhere. Anything that wants to treat groups the way contacts are treated has to walk them
 * separately, and the walk is the one gowhatsapp_find_blist_chat already does.
 */
void gowhatsapp_for_all_chats(PurpleAccount *account, void(*func)(PurpleAccount *, PurpleChat *)) {
    g_return_if_fail(account != NULL);

    for (PurpleBlistNode *node = purple_blist_get_root();
         node != NULL;
         node = purple_blist_node_next(node, TRUE)) {
        if (PURPLE_IS_CHAT(node)) {
            PurpleChat *chat = PURPLE_CHAT(node);

            if (purple_chat_get_account(chat) == account) {
                func(account, chat);
            }
        }
    }
}

// Group chat related functions

/*
 * Add group chat to blist. Updates existing group chat if found. 
 * Only changes blist if fetch contacts is set.
 */
PurpleChat * gowhatsapp_ensure_group_chat_in_blist(PurpleAccount *account, const char *remoteJid, const char *topic) {
    PurpleChat *chat = purple_blist_find_chat(account, remoteJid);

    if (chat == NULL) {
        GHashTable *comp = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free); // MEMCHECK: purple_chat_new takes ownership
        g_hash_table_insert(comp, "name", g_strdup(remoteJid)); // MEMCHECK: g_strdup'ed string released by GHashTable's value_destroy_func g_free (see above)
        chat = purple_blist_chat_new(account, remoteJid, comp); // MEMCHECK: blist takes ownership // TODO: double check if the middle parameter should be the topic instead
        PurpleGroup *group = gowhatsapp_get_purple_group();
        purple_blist_add_chat(chat, group, NULL);
    }

    if (topic != NULL) {
        purple_blist_alias_chat(chat, topic);
    }

    return chat;
}

/*
 * Find group chat in blist.
 * 
 * This reimplements the default behaviour of purple_blist_find_chat 
 * in libpurple/blist.c and could be removed from here.
 * Difference: purple_blist_find_chat returns NULL when account is not connected.
 * 
 * Largely borrowed from:
 * https://github.com/EionRobb/purple-discord/blob/master/libdiscord.c
 */
PurpleChat * 
gowhatsapp_find_blist_chat(PurpleAccount *account, const char *jid) 
{
    PurpleBlistNode *node;

    for (node = purple_blist_get_root();
        node != NULL;
        node = purple_blist_node_next(node, TRUE)) {
        if (PURPLE_IS_CHAT(node)) {
            PurpleChat *chat = PURPLE_CHAT(node);

            if (purple_chat_get_account(chat) != account) {
                continue;
            }

            GHashTable *components = purple_chat_get_components(chat);
            const gchar *chat_jid = g_hash_table_lookup(components, "name");

            if (purple_strequal(chat_jid, jid)) {
                return chat;
            }
        }
    }

    return NULL;
}

/*
 * Returns the alias of the contact or NULL.
 *
 * The alias is owned by the blist and must not be released.
 */
const char * gowhatsapp_blist_get_alias(PurpleAccount *account, const char *who) {
    PurpleBuddy *buddy = purple_blist_find_buddy(account, who);
    if (buddy == NULL) {
        return NULL;
    } else {
        return purple_buddy_get_alias(buddy);
    }
}