package main

/*
#include "constants.h"
#include "bridge.h"
*/
import "C"

import (
	"context"
	"sort"
	"time"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waHistorySync"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/types"
)

/*
 * Asks the primary device for the messages preceding the newest message this
 * client knows of the given conversation. The phone answers asynchronously
 * (and only while WhatsApp is running there) with an events.HistorySync of
 * type ON_DEMAND, handled by handle_history_sync below.
 *
 * The request needs a real anchor message (chat, id, sender and timestamp);
 * the message cache provides it. Without a cached message for the chat there
 * is nothing to anchor on and the request is skipped.
 */
func (handler *Handler) request_history(chatJid string, count int) {
	chat, err := types.ParseJID(chatJid)
	if err != nil || count <= 0 {
		return
	}
	if handler.client.Store.ID == nil {
		// not linked to a phone yet, so there is nobody to ask
		return
	}
	if last, ok := handler.historyRequests[chatJid]; ok && time.Since(last) < time.Minute {
		// a conversation being re-created in quick succession is not a new request
		return
	}
	handler.historyRequests[chatJid] = time.Now()
	anchor := handler.newest_cached_message(chat)
	if anchor == nil {
		handler.log.Infof("No cached message to anchor a history request for %s on.", chatJid)
		return
	}
	ownJid := handler.client.Store.ID.ToNonAD()
	info := types.MessageInfo{
		MessageSource: types.MessageSource{
			Chat:     anchor.Chat,
			Sender:   anchor.Sender,
			IsFromMe: anchor.Sender.ToNonAD() == ownJid,
			IsGroup:  anchor.Chat.Server == types.GroupServer,
		},
		ID:        anchor.ID,
		Timestamp: anchor.Timestamp,
	}
	request := handler.client.BuildHistorySyncRequest(&info, count)
	go func() {
		_, err := handler.client.SendPeerMessage(context.Background(), request)
		if err != nil {
			handler.log.Warnf("Unable to request history for %s: %v", chatJid, err)
		}
	}()
}

func (handler *Handler) newest_cached_message(chat types.JID) *CachedMessage {
	// the cache appends chronologically, so the last match is the newest
	for i := len(handler.cachedMessages) - 1; i >= 0; i-- {
		if handler.cachedMessages[i].Chat.ToNonAD() == chat.ToNonAD() {
			return &handler.cachedMessages[i]
		}
	}
	return nil
}

// one conversation line reconstructed from the phone's history or the cache
type historyEntry struct {
	id        string
	sender    types.JID
	timestamp time.Time
	text      string
	pushName  string
	webMsg    *waWeb.WebMessageInfo // nil for entries replayed from the cache
}

/*
 * The phone's answer to request_history: conversations full of WebMessageInfo.
 * Each is turned back into a displayable line, merged with what the cache
 * knows (the anchor and anything newer), sorted chronologically and displayed
 * as history. Afterwards the state the phone remembers about each message is
 * reported through the same signals live events use: reactions, and delivery
 * or read confirmations for own messages.
 */
func (handler *Handler) handle_history_sync(data *waHistorySync.HistorySync) {
	if handler.client.Store.ID == nil {
		return
	}
	for _, conversation := range data.GetConversations() {
		chat, err := types.ParseJID(conversation.GetID())
		if err != nil {
			handler.log.Warnf("History sync for unparseable conversation %s: %v", conversation.GetID(), err)
			continue
		}
		chat = handler.lidToPn(chat, "handling history conversation")
		isGroup := chat.Server == types.GroupServer

		entries := []historyEntry{}
		seen := map[string]bool{}
		for _, historyMsg := range conversation.GetMessages() {
			webMsg := historyMsg.GetMessage()
			evt, err := handler.client.ParseWebMessage(chat, webMsg)
			if err != nil {
				handler.log.Warnf("Dropping unparseable history message in %s: %v", chat, err)
				continue
			}
			text := history_text(evt.Message)
			if text == "" {
				continue
			}
			sender := handler.lidToPn(evt.Info.Sender, "handling history sender")
			entries = append(entries, historyEntry{
				id:        evt.Info.ID,
				sender:    sender,
				timestamp: evt.Info.Timestamp,
				text:      text,
				pushName:  evt.Info.PushName,
				webMsg:    webMsg,
			})
			seen[evt.Info.ID] = true
		}

		// the phone sends what lies before the anchor; the anchor itself and
		// anything newer the cache remembers fills the gap up to the present
		for i := range handler.cachedMessages {
			cached := &handler.cachedMessages[i]
			if cached.Chat.ToNonAD() != chat.ToNonAD() || seen[cached.ID] {
				continue
			}
			text := history_text(&cached.Message)
			if text == "" {
				continue
			}
			entries = append(entries, historyEntry{
				id:        cached.ID,
				sender:    cached.Sender,
				timestamp: cached.Timestamp,
				text:      text,
			})
		}

		sort.SliceStable(entries, func(i, j int) bool {
			return entries[i].timestamp.Before(entries[j].timestamp)
		})

		ownJid := handler.client.Store.ID.ToNonAD()
		for _, entry := range entries {
			var pushName *string
			if entry.pushName != "" {
				name := entry.pushName
				pushName = &name
			}
			purple_display_history_message(handler.account, chat.ToNonAD().String(), isGroup, entry.sender.ToNonAD().String(), pushName, entry.timestamp, entry.text, entry.id)
		}
		for _, entry := range entries {
			if entry.webMsg == nil {
				continue
			}
			handler.report_history_state(chat, isGroup, ownJid, entry)
		}
	}
}

/*
 * Reports what the phone remembers about one historical message: reactions it
 * gathered, and how far an own message travelled (delivered, read). These use
 * the same signals as live receipts and reactions, so a front-end that draws
 * ticks and reaction chips restores them on the historical lines too.
 */
func (handler *Handler) report_history_state(chat types.JID, isGroup bool, ownJid types.JID, entry historyEntry) {
	chatJid := chat.ToNonAD().String()
	for _, reaction := range entry.webMsg.GetReactions() {
		emoji := reaction.GetText()
		if emoji == "" {
			// a removed reaction was never shown; nothing to take back
			continue
		}
		var sender types.JID
		key := reaction.GetKey()
		if key.GetFromMe() {
			sender = ownJid
		} else if participant := key.GetParticipant(); participant != "" {
			parsed, err := types.ParseJID(participant)
			if err != nil {
				continue
			}
			sender = handler.lidToPn(parsed, "handling history reaction sender")
		} else {
			sender = chat
		}
		timestamp := time.UnixMilli(reaction.GetSenderTimestampMS())
		purple_reaction(handler.account, chatJid, isGroup, sender.ToNonAD().String(), entry.id, emoji, timestamp)
	}

	if entry.webMsg.GetKey().GetFromMe() {
		var receiptType C.char
		switch entry.webMsg.GetStatus() {
		case waWeb.WebMessageInfo_DELIVERY_ACK:
			receiptType = C.char(C.gowhatsapp_receipt_type_delivered)
		case waWeb.WebMessageInfo_READ, waWeb.WebMessageInfo_PLAYED:
			receiptType = C.char(C.gowhatsapp_receipt_type_read)
		default:
			return
		}
		purple_receipt(handler.account, chatJid, ownJid.ToNonAD().String(), isGroup, entry.timestamp, receiptType, entry.id)
	}
}

/*
 * The line of text a historical message displays as. Text carries over as-is;
 * media becomes a short placeholder (historical attachments are not
 * downloaded), keeping its caption if it had one.
 */
func history_text(message *waE2E.Message) string {
	if message == nil {
		return ""
	}
	if text := message.GetConversation(); text != "" {
		return text
	}
	if text := message.GetExtendedTextMessage().GetText(); text != "" {
		return text
	}
	if m := message.GetImageMessage(); m != nil {
		return history_placeholder("an image", m.GetCaption())
	}
	if m := message.GetVideoMessage(); m != nil {
		return history_placeholder("a video", m.GetCaption())
	}
	if message.GetPtvMessage() != nil {
		return "[Sent a video message.]"
	}
	if message.GetAudioMessage() != nil {
		return "[Sent a voice message.]"
	}
	if message.GetStickerMessage() != nil {
		return "[Sent a sticker.]"
	}
	if m := message.GetDocumentMessage(); m != nil {
		return history_placeholder("a file: "+m.GetFileName(), m.GetCaption())
	}
	return ""
}

func history_placeholder(what string, caption string) string {
	if caption != "" {
		return "[Sent " + what + ".] " + caption
	}
	return "[Sent " + what + ".]"
}
