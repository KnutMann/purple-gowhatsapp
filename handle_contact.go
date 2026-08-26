package main

import (
	"strings"

	"go.mau.fi/whatsmeow/proto/waE2E"
)

/*
 * A shared contact arrives as a ContactMessage (one vCard) or a
 * ContactsArrayMessage (several). Neither carries conversation text, so
 * without this they display as nothing at all. The vCard stays on the phone;
 * what a chat line can usefully carry is the name and the telephone numbers.
 * See https://github.com/hoehermann/purple-gowhatsapp/issues/207.
 */
func contact_message_text(message *waE2E.Message) string {
	if m := message.GetContactMessage(); m != nil {
		return contact_card_text(m)
	}
	if m := message.GetContactsArrayMessage(); m != nil {
		cards := []string{}
		for _, contact := range m.GetContacts() {
			if card := contact_card_text(contact); card != "" {
				cards = append(cards, card)
			}
		}
		return strings.Join(cards, "\n")
	}
	return ""
}

func contact_card_text(m *waE2E.ContactMessage) string {
	name := m.GetDisplayName()
	numbers := vcard_telephone_numbers(m.GetVcard())
	if name == "" && len(numbers) == 0 {
		return ""
	}
	text := "[CONTACT] " + name
	if len(numbers) > 0 {
		if name != "" {
			text += ": "
		}
		text += strings.Join(numbers, ", ")
	}
	return text
}

/*
 * The values of the TEL properties of a vCard, in order. A property line looks
 * like "TEL;type=CELL;waid=49123456789:+49 123 456789" and may carry a group
 * prefix ("item1.TEL;…"). Values are never interpreted, only carried over.
 */
func vcard_telephone_numbers(vcard string) []string {
	numbers := []string{}
	for _, line := range vcard_unfold(vcard) {
		property, value, found := strings.Cut(line, ":")
		if !found || value == "" {
			continue
		}
		name, _, _ := strings.Cut(property, ";")
		if dot := strings.LastIndex(name, "."); dot >= 0 {
			name = name[dot+1:]
		}
		if strings.EqualFold(name, "TEL") {
			numbers = append(numbers, strings.TrimSpace(value))
		}
	}
	return numbers
}

// vCard 3.0 folds long lines by continuing them after CRLF plus one space or
// tab (RFC 2425 section 5.8.1); undo that so every property is one line
func vcard_unfold(vcard string) []string {
	lines := []string{}
	for _, line := range strings.Split(strings.ReplaceAll(vcard, "\r\n", "\n"), "\n") {
		if len(line) > 0 && (line[0] == ' ' || line[0] == '\t') && len(lines) > 0 {
			lines[len(lines)-1] += line[1:]
		} else {
			lines = append(lines, line)
		}
	}
	return lines
}
