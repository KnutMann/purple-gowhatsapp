package main

import (
	"context"
	"time"

	"go.mau.fi/whatsmeow/types"
)

/*
 * This function will try to query group information from WhatsApp servers.
 * Upon immediate success, the list of participants is returned.
 * In case of failure (e.g. due to timeout), the function will return nil and retry in background.
 * Upon delayed success, the response is fed to purple asynchronously.
 */
func (handler *Handler) query_group_participants_retry(group_jid types.JID, seconds_backoff int, max_retries int, retry_count int) []types.GroupParticipant {
	group, err := handler.client.GetGroupInfo(context.TODO(), group_jid)
	if err == nil && group != nil {
		// always feed the full group info (name included) back to purple,
		// so conversation titles resolve even on the first fetch
		purple_update_group(handler.account, group)
		if retry_count == 0 {
			return group.Participants
		}
	} else {
		consequence := "Giving up."
		if retry_count < max_retries {
			consequence = "Retrying…"
			go func() {
				time.Sleep(time.Duration(seconds_backoff) * time.Second)
				handler.query_group_participants_retry(group_jid, seconds_backoff, max_retries, retry_count+1)
			}()
		}
		handler.log.Infof("Cannot get group information due to %#v. %s", err, consequence)
	}
	return []types.GroupParticipant{}
}

/*
 * This function queries the list of joined groups from WhatsApp servers and feeds it to purple.
 * In case of failure (e.g. due to timeout), it waits and retries a few times, then gives up.
 * Giving up is not fatal: the list only decides which group chats are added to the buddy list,
 * and messages flow with or without it. The end-of-list marker is sent even after giving up,
 * or the room list would remain "in progress" and the next query would be refused.
 * Nothing is sent once the connection is gone; the room list it was meant for is gone with it.
 */
func (handler *Handler) query_groups_retry(seconds_backoff int, max_retries int, retry_count int) {
	groups, err := handler.client.GetJoinedGroups(context.TODO())
	if err != nil {
		if !handler.client.IsConnected() {
			handler.log.Infof("Unable to get list of groups due to %v. Not connected, not retrying.", err)
			return
		}
		if retry_count < max_retries {
			handler.log.Infof("Unable to get list of groups due to %v. Retrying…", err)
			time.Sleep(time.Duration(seconds_backoff) * time.Second)
			handler.query_groups_retry(seconds_backoff, max_retries, retry_count+1)
			return
		}
		handler.log.Warnf("Unable to get list of groups due to %v. Giving up.", err)
	}
	for _, group := range groups {
		group.Participants = handler.groupChatParticipantsLidToPn(group.Participants, "group chat query")
		purple_update_group(handler.account, group)
	}
	// emit an empty group message to denote end of list
	purple_update_group(handler.account, nil)
}
