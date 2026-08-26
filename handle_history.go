package main

/*
#include "constants.h"
#include "bridge.h"
*/
import "C"

import (
	"context"
	"encoding/hex"
	"fmt"
	"html"
	"os"
	"path/filepath"
	"sort"
	"strings"
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
	for jid, when := range handler.historyRequests {
		// an entry past its minute holds nothing back and need not be kept
		if time.Since(when) > time.Minute {
			delete(handler.historyRequests, jid)
		}
	}
	handler.historyRequests[chatJid] = time.Now()

	/* The request points at a message and asks for what came before it. The
	 * cache supplies that anchor where it can; a conversation nobody has spoken
	 * in since this client started is not in the cache, and rather than give up
	 * there the request points at the present moment and names no message. The
	 * phone decides whether it will answer that. */
	info := types.MessageInfo{
		MessageSource: types.MessageSource{
			Chat:    chat,
			IsGroup: chat.Server == types.GroupServer,
		},
		Timestamp: time.Now(),
	}
	if anchor := handler.newest_cached_message(chat); anchor != nil {
		/* Everything but the conversation itself comes from the anchor. The
		 * conversation stays the one that was parsed and asked about: the cache
		 * records a chat as the message path left it, which for a status
		 * broadcast is a particular device rather than the conversation, and
		 * the request goes out with no device suffix trimmed off it. */
		ownJid := handler.client.Store.ID.ToNonAD()
		info.Sender = anchor.Sender
		info.IsFromMe = anchor.Sender.ToNonAD() == ownJid
		info.ID = anchor.ID
		info.Timestamp = anchor.Timestamp
	} else {
		handler.log.Infof("Requesting history for %s with no anchor message; the cache knows none.", chatJid)
	}
	request := handler.client.BuildHistorySyncRequest(&info, count)
	go func() {
		_, err := handler.client.SendPeerMessage(context.Background(), request)
		if err != nil {
			handler.log.Warnf("Unable to request history for %s: %v", chatJid, err)
		}
	}()
}

/*
 * The cache belongs to the connection's own goroutine, and both callers here
 * reach it from purple's thread, so what they take away is a copy: a pointer
 * into the slice would outlive the next trim.
 */
func (handler *Handler) newest_cached_message(chat types.JID) *CachedMessage {
	handler.cacheMutex.Lock()
	defer handler.cacheMutex.Unlock()
	// the cache appends chronologically, so the last match is the newest
	for i := len(handler.cachedMessages) - 1; i >= 0; i-- {
		if handler.cachedMessages[i].Chat.ToNonAD() == chat.ToNonAD() {
			found := handler.cachedMessages[i]
			return &found
		}
	}
	return nil
}

// every message the cache holds for one conversation, oldest first
func (handler *Handler) cached_messages_for(chat types.JID) []CachedMessage {
	handler.cacheMutex.Lock()
	defer handler.cacheMutex.Unlock()
	found := []CachedMessage{}
	for i := range handler.cachedMessages {
		if handler.cachedMessages[i].Chat.ToNonAD() == chat.ToNonAD() {
			found = append(found, handler.cachedMessages[i])
		}
	}
	return found
}

// one conversation line reconstructed from the phone's history or the cache
type historyEntry struct {
	id        string
	sender    types.JID
	timestamp time.Time
	text      string
	isHTML    bool // the text is a ready-made media link, not prose to escape
	pushName  string
	webMsg    *waWeb.WebMessageInfo // nil for entries replayed from the cache
}

// a mark to put on one of those lines rather than a line of its own
type historyReaction struct {
	targetId  string
	sender    types.JID
	emoji     string // empty where the reaction was taken back again
	timestamp time.Time
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
	// the phone answers asynchronously or not at all, so say when it did
	handler.log.Infof("History sync answer for %d conversation(s).", len(data.GetConversations()))
	ownJid := handler.client.Store.ID.ToNonAD()
	for _, conversation := range data.GetConversations() {
		chat, err := types.ParseJID(conversation.GetID())
		if err != nil {
			handler.log.Warnf("History sync for unparseable conversation %s: %v", conversation.GetID(), err)
			continue
		}
		chat = handler.lidToPn(chat, "handling history conversation")
		isGroup := chat.Server == types.GroupServer

		entries := []historyEntry{}
		reactions := []historyReaction{}
		seen := map[string]bool{}
		var newestFromPhone time.Time
		var unshowable int
		for _, historyMsg := range conversation.GetMessages() {
			webMsg := historyMsg.GetMessage()
			evt, err := handler.client.ParseWebMessage(chat, webMsg)
			if err != nil {
				handler.log.Warnf("Dropping unparseable history message in %s: %v", chat, err)
				continue
			}
			/* Our own messages name their author in whatever identity the phone
			 * recorded at the time, which for some is the hidden lid rather than
			 * the phone number, and no mapping brings that one home: it is our
			 * own, and the store maps other people's. A room would then seat us
			 * twice, under two names, one of them a number nobody recognises. */
			sender := ownJid
			if !evt.Info.IsFromMe {
				sender = handler.lidToPn(evt.Info.Sender, "handling history sender")
			}

			/* A reaction is a message in its own right here, naming the message
			 * it belongs to. It is not a line to show but a mark to put on one,
			 * so it is set aside until the lines are on the page. */
			if rm := evt.Message.GetReactionMessage(); rm != nil && rm.Key != nil && rm.Key.ID != nil {
				reactions = append(reactions, historyReaction{
					targetId:  rm.Key.GetID(),
					sender:    sender,
					emoji:     rm.GetText(),
					timestamp: evt.Info.Timestamp,
				})
				continue
			}

			text, isHTML := history_line(evt.Message)
			if text == "" {
				unshowable++
				continue
			}
			/* An edited message is parsed under the id of the message it edits,
			 * which is the right id to carry but would put two lines on the page
			 * claiming to be the same message. The later one wins. */
			if seen[evt.Info.ID] {
				for i := range entries {
					if entries[i].id == evt.Info.ID {
						entries = append(entries[:i], entries[i+1:]...)
						break
					}
				}
			}
			entries = append(entries, historyEntry{
				id:        evt.Info.ID,
				sender:    sender,
				timestamp: evt.Info.Timestamp,
				text:      text,
				isHTML:    isHTML,
				pushName:  evt.Info.PushName,
				webMsg:    webMsg,
			})
			seen[evt.Info.ID] = true
			/* Into the cache under its id, so replies and reactions can name a
			 * history line as readily as a live one; ids already cached are
			 * skipped there, and one persist at the end covers the burst. */
			handler.add_to_cache_unpersisted(evt.Message, evt.Info.ID, chat, sender, evt.Info.Timestamp)
			if evt.Info.Timestamp.After(newestFromPhone) {
				newestFromPhone = evt.Info.Timestamp
			}
		}

		handler.log.Infof("History for %s: %d in the answer, %d to show, %d reaction(s), %d with nothing to show.",
			chat, len(conversation.GetMessages()), len(entries), len(reactions), unshowable)

		/* The phone answers with what lies before the anchor and stops there, so
		 * the anchor itself and anything after it is missing from its answer.
		 * The cache fills exactly that gap, and nothing else: an older message
		 * the cache happens to hold is one the phone chose not to send, and it
		 * is not this client's place to overrule that. An answer that brought no
		 * messages marks no gap, so the cache stays out of it entirely. */
		if len(entries) > 0 {
			for _, cached := range handler.cached_messages_for(chat) {
				if seen[cached.ID] || cached.Timestamp.Before(newestFromPhone) {
					continue
				}
				text, isHTML := history_line(&cached.Message)
				if text == "" {
					continue
				}
				entries = append(entries, historyEntry{
					id:        cached.ID,
					sender:    cached.Sender,
					timestamp: cached.Timestamp,
					text:      text,
					isHTML:    isHTML,
				})
			}
		}

		sort.SliceStable(entries, func(i, j int) bool {
			return entries[i].timestamp.Before(entries[j].timestamp)
		})

		// the user asked for a number of messages, so show that many, the newest
		if wanted := purple_get_int(handler.account, C.GOWHATSAPP_FETCH_HISTORY_OPTION, 0); wanted > 0 && len(entries) > wanted {
			entries = entries[len(entries)-wanted:]
		}

		for _, entry := range entries {
			var pushName *string
			if entry.pushName != "" {
				name := entry.pushName
				pushName = &name
			}
			purple_display_history_message(handler.account, chat.ToNonAD().String(), isGroup, entry.sender.ToNonAD().String(), pushName, entry.timestamp, entry.text, entry.id, entry.isHTML)
		}
		for _, entry := range entries {
			if entry.webMsg == nil {
				continue
			}
			handler.report_history_state(chat, isGroup, ownJid, entry)
		}

		/* Oldest first, so that where a reaction was changed or taken back the
		 * last word is the one that stands. */
		sort.SliceStable(reactions, func(i, j int) bool {
			return reactions[i].timestamp.Before(reactions[j].timestamp)
		})
		for _, reaction := range reactions {
			purple_reaction(handler.account, chat.ToNonAD().String(), isGroup, reaction.sender.ToNonAD().String(), reaction.targetId, reaction.emoji, reaction.timestamp)
		}
	}
	handler.persist_cache()
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
 * The line a historical message displays as, preferring substance over a
 * placeholder: where the live path already downloaded the attachment - videos
 * and images stay in the temporary directory under content-hash names, voice
 * notes leave their decoded WAV there - the line becomes the same media link
 * the live message showed, and the front-end swaps it for a player or a
 * picture again. Where no file is lying around (never received live, or the
 * directory was cleaned), the plain placeholder stands as before.
 *
 * The link is ready-made HTML and says so, since everything else this file
 * emits is prose for the display path to escape.
 */
func history_line(message *waE2E.Message) (string, bool) {
	if link := history_media_link(message); link != "" {
		return link, true
	}
	return history_text(message), false
}

// the media file the live path would have left behind for this message, or ""
func history_media_file(message *waE2E.Message) (path string, label string, caption string) {
	if m := message.GetVideoMessage(); m != nil && len(m.GetFileSHA256()) > 0 {
		name := "AdiumVideo_" + hex.EncodeToString(m.GetFileSHA256()) + extension_from_mimetype(m.Mimetype)
		return filepath.Join(os.TempDir(), name), "Video message", m.GetCaption()
	}
	if m := message.GetPtvMessage(); m != nil && len(m.GetFileSHA256()) > 0 {
		name := "AdiumVideo_" + hex.EncodeToString(m.GetFileSHA256()) + extension_from_mimetype(m.Mimetype)
		return filepath.Join(os.TempDir(), name), "Video message", m.GetCaption()
	}
	if m := message.GetAudioMessage(); m != nil && len(m.GetFileSHA256()) > 0 {
		name := "AdiumVoice_" + hex.EncodeToString(m.GetFileSHA256()) + ".wav"
		return filepath.Join(os.TempDir(), name), "Voice message", ""
	}
	if m := message.GetImageMessage(); m != nil && len(m.GetFileSHA256()) > 0 {
		name := "whatsapp_image_" + hex.EncodeToString(m.GetFileSHA256()) + extension_from_mimetype(m.Mimetype)
		return filepath.Join(os.TempDir(), name), "Image", m.GetCaption()
	}
	return "", "", ""
}

func history_media_link(message *waE2E.Message) string {
	path, label, caption := history_media_file(message)
	if path == "" {
		return ""
	}
	if _, err := os.Stat(path); err != nil {
		return ""
	}
	link := fmt.Sprintf("<a href=\"file://%s\">%s</a>", path, label)
	if caption != "" {
		//The caption is the sender's text and must not become markup
		link += " " + strings.ReplaceAll(html.EscapeString(caption), "\n", "<br>")
	}
	return link
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
	/* The kinds the live path spells out in full and this one need only name, so
	 * that a conversation replayed from the phone has no silent holes in it. */
	if m := message.GetLocationMessage(); m != nil {
		label := m.GetName()
		if label == "" {
			label = m.GetAddress()
		}
		if label == "" {
			label = "Shared location"
		}
		return fmt.Sprintf("%s: https://maps.apple.com/?ll=%f,%f&q=%f,%f", label,
			m.GetDegreesLatitude(), m.GetDegreesLongitude(),
			m.GetDegreesLatitude(), m.GetDegreesLongitude())
	}
	if m := message.GetLiveLocationMessage(); m != nil {
		return fmt.Sprintf("Shared live location (last position: https://maps.apple.com/?ll=%f,%f&q=%f,%f)",
			m.GetDegreesLatitude(), m.GetDegreesLongitude(),
			m.GetDegreesLatitude(), m.GetDegreesLongitude())
	}
	if m := GetAnyPollCreationMessage(message); m != nil {
		return "[POLL] " + m.GetName()
	}
	if text := contact_message_text(message); text != "" {
		return text
	}
	/* Everything else, group events and calls and votes among them, has nothing
	 * to say as a line of conversation and is left out on purpose. */
	return ""
}

func history_placeholder(what string, caption string) string {
	if caption != "" {
		return "[Sent " + what + ".] " + caption
	}
	return "[Sent " + what + ".]"
}
