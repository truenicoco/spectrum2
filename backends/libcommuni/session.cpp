/**
 * XMPP - libpurple transport
 *
 * Copyright (C) 2013, Jan Kaluza <hanzz.k@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include "session.h"
#include <QtCore>
#include <iostream>
#include <IrcCommand>
#include <IrcMessage>
#include "backports.h"

#include "ircnetworkplugin.h"

#define FROM_UTF8(WHAT) QString::fromUtf8((WHAT).c_str(), (WHAT).size())
#define TO_UTF8(WHAT) std::string((WHAT).toUtf8().data(), (WHAT).toUtf8().size())

#include "transport/Logging.h"

DEFINE_LOGGER(logger, "IRCConnection");

// static bool sentList;

MyIrcSession::MyIrcSession(const std::string &user, IRCNetworkPlugin *np, const std::string &suffix, QObject* parent) : IrcConnection(parent)
{
	m_np = np;
	m_user = user;
	m_suffix = suffix;
	m_connected = false;
	rooms = 0;

	connect(this, SIGNAL(disconnected()), SLOT(on_disconnected()));
	connect(this, SIGNAL(socketError(QAbstractSocket::SocketError)), SLOT(on_socketError(QAbstractSocket::SocketError)));
	connect(this, SIGNAL(connected()), SLOT(on_connected()));
	connect(this, SIGNAL(messageReceived(IrcMessage*)), this, SLOT(onMessageReceived(IrcMessage*)));

	m_awayTimer = new QTimer(this);
	connect(m_awayTimer, SIGNAL(timeout()), this, SLOT(awayTimeout()));
	m_awayTimer->start(5*1000);
}

MyIrcSession::~MyIrcSession() {
	delete m_awayTimer;
}

void MyIrcSession::on_connected() {
	m_connected = true;
	if (m_suffix.empty()) {
		m_np->handleConnected(m_user);
// 		if (!sentList) {
// 			sendCommand(IrcCommand::createList("", ""));
// 			sentList = true;
// 		}
	}

// 	sendCommand(IrcCommand::createCapability("REQ", QStringList("away-notify")));

	for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
		sendCommand(IrcCommand::createJoin(FROM_UTF8(it->second->getChannel()), FROM_UTF8(it->second->getPassword())));
	}

	if (getIdentify().find(" ") != std::string::npos) {
		std::string to = getIdentify().substr(0, getIdentify().find(" "));
		std::string what = getIdentify().substr(getIdentify().find(" ") + 1);
		LOG4CXX_INFO(logger, m_user << ": Sending IDENTIFY message to " << to);
		sendCommand(IrcCommand::createMessage(FROM_UTF8(to), FROM_UTF8(what)));
	}
}

void MyIrcSession::addPM(const std::string &name, const std::string &room) {
	LOG4CXX_INFO(logger, m_user << ": Adding PM conversation " << name << " " << room);
	m_pms[name] = room;
}

void MyIrcSession::on_socketError(QAbstractSocket::SocketError error) {
	std::string reason;
	switch(error) {
		case QAbstractSocket::ConnectionRefusedError: reason = "The connection was refused by the peer (or timed out)."; break;
		case QAbstractSocket::RemoteHostClosedError: reason = "The remote host closed the connection."; break;
		case QAbstractSocket::HostNotFoundError: reason = "The host address was not found."; break;
		case QAbstractSocket::SocketAccessError: reason = "The socket operation failed because the application lacked the required privileges."; break;
		case QAbstractSocket::SocketResourceError: reason = "The local system ran out of resources."; break;
		case QAbstractSocket::SocketTimeoutError: reason = "The socket operation timed out."; break;
		case QAbstractSocket::DatagramTooLargeError: reason = "The datagram was larger than the operating system's limit."; break;
		case QAbstractSocket::NetworkError: reason = "An error occurred with the network."; break;
		case QAbstractSocket::SslHandshakeFailedError: reason = "The SSL/TLS handshake failed, so the connection was closed"; break;
		case QAbstractSocket::UnknownSocketError: reason = "An unidentified error occurred."; break;
		default: reason= "Unknown error."; break;
	};

	if (!m_suffix.empty()) {
		for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
			m_np->handleParticipantChanged(m_user, TO_UTF8(nickName()), it->second->getChannel() + m_suffix, pbnetwork::PARTICIPANT_FLAG_ROOM_NOT_FOUND, pbnetwork::STATUS_NONE, reason);
		}
	}
	else {
		m_np->handleDisconnected(m_user, 0, reason);
		m_np->tryNextServer();
	}
	m_connected = false;
}

void MyIrcSession::on_disconnected() {
	if (m_suffix.empty()) {
		m_np->handleDisconnected(m_user, 0, "");
		m_np->tryNextServer();
	}
	m_connected = false;
}

bool MyIrcSession::correctNickname(std::string &nickname) {
	bool flags = 0;
	if (!nickname.empty()) { 
		switch(nickname.at(0)) {
			case '@': nickname = nickname.substr(1); flags = 1; break;
			case '+': nickname = nickname.substr(1); break;
			case '~': nickname = nickname.substr(1); break;
			case '&': nickname = nickname.substr(1); break;
			case '%': nickname = nickname.substr(1); break;
			default: break;
		}
	}
	return flags;
}

void MyIrcSession::on_joined(IrcMessage *message) {
	IrcJoinMessage *m = (IrcJoinMessage *) message;
	std::string nickname = TO_UTF8(m->nick());
	bool op = correctNickname(nickname);
	getIRCBuddy(TO_UTF8(m->channel().toLower()), nickname).setOp(op);
	m_np->handleParticipantChanged(m_user, nickname, TO_UTF8(m->channel().toLower()) + m_suffix, op, pbnetwork::STATUS_ONLINE);
	LOG4CXX_INFO(logger, m_user << ": " << nickname << " joined " << TO_UTF8(m->channel().toLower()) + m_suffix);
}


void MyIrcSession::on_parted(IrcMessage *message) {
	IrcPartMessage *m = (IrcPartMessage *) message;
	std::string nickname = TO_UTF8(m->nick());
	bool op = correctNickname(nickname);
	removeIRCBuddy(TO_UTF8(m->channel().toLower()), nickname);
	LOG4CXX_INFO(logger, m_user << ": " << nickname << " parted " << TO_UTF8(m->channel().toLower()) + m_suffix);
	m_np->handleParticipantChanged(m_user, nickname, TO_UTF8(m->channel().toLower()) + m_suffix, op, pbnetwork::STATUS_NONE, TO_UTF8(m->reason()));
}

void MyIrcSession::on_quit(IrcMessage *message) {
	IrcQuitMessage *m = (IrcQuitMessage *) message;
	std::string nickname = TO_UTF8(m->nick());
	bool op = correctNickname(nickname);

	for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
		if (!hasIRCBuddy(it->second->getChannel(), nickname)) {
			continue;
		}
		removeIRCBuddy(it->second->getChannel(), nickname);
		LOG4CXX_INFO(logger, m_user << ": " << nickname << " quit " << it->second->getChannel() + m_suffix);
		m_np->handleParticipantChanged(m_user, nickname, it->second->getChannel() + m_suffix, op, pbnetwork::STATUS_NONE, TO_UTF8(m->reason()));
	}
}

void MyIrcSession::on_nickChanged(IrcMessage *message) {
	IrcNickMessage *m = (IrcNickMessage *) message;
	std::string nickname = TO_UTF8(m->nick());
	correctNickname(nickname);

	for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
		if (!hasIRCBuddy(it->second->getChannel(), nickname)) {
			continue;
		}
		IRCBuddy &buddy = getIRCBuddy(it->second->getChannel(), nickname);
		LOG4CXX_INFO(logger, m_user << ": " << nickname << " changed nickname to " << TO_UTF8(m->nick()));
		m_np->handleParticipantChanged(m_user, nickname, it->second->getChannel() + m_suffix,(int) buddy.isOp(), pbnetwork::STATUS_ONLINE, "", TO_UTF8(m->nick()));
	}
}

void MyIrcSession::on_modeChanged(IrcMessage *message) {
	IrcModeMessage *m = (IrcModeMessage *) message;

	// mode changed: "#testik" "HanzZ" "+o" "hanzz_k"
	std::string nickname = TO_UTF8(m->argument());
	std::string mode = TO_UTF8(m->mode());
	if (nickname.empty())
		return;

	correctNickname(nickname);

	if (!hasIRCBuddy(TO_UTF8(m->target().toLower()), nickname)) {
		return;
	}
	IRCBuddy &buddy = getIRCBuddy(TO_UTF8(m->target().toLower()), nickname);
	if (mode == "+o") {
		buddy.setOp(true);
	}
	else {
		buddy.setOp(false);
	}
	
	m_np->handleParticipantChanged(m_user, nickname, TO_UTF8(m->target().toLower()) + m_suffix,(int) buddy.isOp(), pbnetwork::STATUS_ONLINE, "");

	LOG4CXX_INFO(logger, m_user << ": " << nickname << " changed mode to " << mode << " in " << TO_UTF8(m->target().toLower()));
}

void MyIrcSession::on_topicChanged(IrcMessage *message) {
	IrcTopicMessage *m = (IrcTopicMessage *) message;

	std::string nickname = TO_UTF8(m->nick());
	correctNickname(nickname);

	LOG4CXX_INFO(logger, m_user << ": " << nickname << " topic changed to " << TO_UTF8(m->topic()));
	m_np->handleSubject(m_user, TO_UTF8(m->channel().toLower()) + m_suffix, TO_UTF8(m->topic()), nickname);
}

void MyIrcSession::sendWhoisCommand(const std::string &channel, const std::string &to) {
	m_whois[to] = channel;
	sendCommand(IrcCommand::createWhois(FROM_UTF8(to)));
}

void MyIrcSession::on_whoisMessageReceived(IrcMessage *message) {
	IrcWhoisMessage *m = (IrcWhoisMessage *) message;
	std::string nickname = TO_UTF8(m->nick());
	if (m_whois.find(nickname) == m_whois.end()) {
		LOG4CXX_INFO(logger, "Whois response received with unexpected nickname " << nickname);
		return;
	}

	std::string msg = "";
	msg += nickname + " is connected to " + TO_UTF8(m->server()) + " (" + TO_UTF8(m->realName()) + ")\n";
	msg += nickname + " is a user on channels: " + TO_UTF8(m->channels().join(", "));

	sendMessageToFrontend(m_whois[nickname], "whois", msg);
	m_whois.erase(nickname);
}

void MyIrcSession::sendMessageToFrontend(const std::string &channel, const std::string &nick, const std::string &msg) {
	QString html = "";//msg;
// 	CommuniBackport::toPlainText(msg);

	// TODO: Communi produces invalid html now...
// 	if (html == msg) {
// 		html = "";
// 	}
// 	else {
// 		html = IrcUtil::messageToHtml(html);
// 	}

	std::string nickname = nick;
	if (channel.find("#") == 0) {
		correctNickname(nickname);
		m_np->handleMessage(m_user, channel + m_suffix, msg, nickname, TO_UTF8(html));
	}
	else {
		correctNickname(nickname);
		if (m_pms.find(nickname) != m_pms.end()) {
			std::string room = m_pms[nickname].substr(0, m_pms[nickname].find("/"));
			room = room.substr(0, room.find("@"));
			if (hasIRCBuddy(room, nickname)) {
				m_np->handleMessage(m_user, room + m_suffix, msg, nickname, TO_UTF8(html), "", false, true);
				return;
			}
			else {
				nickname = nickname + m_suffix;
			}
		}
		else {
			for (AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
				if (!hasIRCBuddy(it->second->getChannel(), nickname)) {
					continue;
				}
				addPM(nickname, it->second->getChannel());
				m_np->handleMessage(m_user, it->second->getChannel() + m_suffix, msg, nickname, TO_UTF8(html), "", false, true);
				return;
			}

			nickname = nickname + m_suffix;
		}

		m_np->handleMessage(m_user, nickname, msg, "", TO_UTF8(html));
	}
}

void MyIrcSession::on_messageReceived(IrcMessage *message) {
	IrcPrivateMessage *m = (IrcPrivateMessage *) message;
	if (m->isRequest()) {
		QString request = m->content().split(" ", QString::SkipEmptyParts).value(0).toUpper();
		if (request == "PING" || request == "TIME" || request == "VERSION") {
			LOG4CXX_INFO(logger, m_user << ": " << TO_UTF8(request) << " received and has been answered");
			return;
		}
	}

	std::string msg = TO_UTF8(m->content());
	if (m->isAction()) {
		msg = "/me " + msg;
	}

	std::string target = TO_UTF8(m->target().toLower());
	std::string nickname = TO_UTF8(m->nick());
	sendMessageToFrontend(target, nickname, msg);
}

void MyIrcSession::on_numericMessageReceived(IrcMessage *message) {
	QString channel;
	QStringList members;
	std::string nick;

	IrcNumericMessage *m = (IrcNumericMessage *) message;
	QStringList parameters = m->parameters();
	switch (m->code()) {
		case 301:
			break;
		case 315:
			LOG4CXX_INFO(logger, "End of /who request " << TO_UTF8(parameters[1]));
			break;
		case 332:
			m_topicData = TO_UTF8(parameters[2]);
			break;
		case 333:
			nick = TO_UTF8(parameters[2]);
			if (nick.find("!") != std::string::npos) {
				nick = nick.substr(0, nick.find("!"));
			}
			if (nick.find("/") != std::string::npos) {
				nick = nick.substr(0, nick.find("/"));
			}
			m_np->handleSubject(m_user, TO_UTF8(parameters[1].toLower()) + m_suffix, m_topicData, nick);
			break;
		case 352: {
			channel = parameters[1].toLower();
			nick = TO_UTF8(parameters[5]);
			IRCBuddy &buddy = getIRCBuddy(TO_UTF8(channel), nick);

			if (parameters[6].toUpper().startsWith("G")) {
				if (!buddy.isAway()) {
					buddy.setAway(true);
					m_np->handleParticipantChanged(m_user, nick, TO_UTF8(channel) + m_suffix, buddy.isOp(), pbnetwork::STATUS_AWAY);
				}
			}
			else if (buddy.isAway()) {
				buddy.setAway(false);
				m_np->handleParticipantChanged(m_user, nick, TO_UTF8(channel) + m_suffix, buddy.isOp(), pbnetwork::STATUS_ONLINE);
			}
			break;
		}
		case 353:
			channel = parameters[2].toLower();
			members = parameters[3].split(" ");

			LOG4CXX_INFO(logger, m_user << ": Received members for " << TO_UTF8(channel) << m_suffix);
			for (int i = 0; i < members.size(); i++) {
				bool op = 0;
				std::string nickname = TO_UTF8(members.at(i));
				op = correctNickname(nickname);
				IRCBuddy &buddy = getIRCBuddy(TO_UTF8(channel), nickname);
				buddy.setOp(op);
				m_np->handleParticipantChanged(m_user, nickname, TO_UTF8(channel) + m_suffix, buddy.isOp(), pbnetwork::STATUS_ONLINE);
			}

			break;
		case 366:
			// ask /who to get away states
			channel = parameters[1].toLower();
			LOG4CXX_INFO(logger, m_user << "Asking /who for channel " << TO_UTF8(channel));
			sendCommand(IrcCommand::createWho(channel));
			break;
		case 401:
		case 402:
			nick = TO_UTF8(parameters[1]);
			if (m_whois.find(nick) != m_whois.end()) {
				sendMessageToFrontend(m_whois[nick], "whois", nick + ": No such client");
				m_whois.erase(nick);
			}
			break;
		case 432:
			m_np->handleDisconnected(m_user, pbnetwork::CONNECTION_ERROR_INVALID_USERNAME, "Erroneous Nickname");
			break;
		case 433:
			for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
				m_np->handleRoomNicknameChanged(m_user, it->second->getChannel() + m_suffix, TO_UTF8(nickName() + "_"));
				m_np->handleParticipantChanged(m_user, TO_UTF8(nickName()), it->second->getChannel() + m_suffix, 0, pbnetwork::STATUS_ONLINE, "", TO_UTF8(nickName() + "_"));
			}
			setNickName(nickName() + "_");
			open();
// 			for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
// 				m_np->handleParticipantChanged(m_user, TO_UTF8(nickName()), it->second->getChannel() + m_suffix, pbnetwork::PARTICIPANT_FLAG_CONFLICT);
// 			}
// 			if (m_suffix.empty()) {
// 				m_np->handleDisconnected(m_user, pbnetwork::CONNECTION_ERROR_INVALID_USERNAME, "Nickname is already in use");
// 			}
			break;
		case 436:
			for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
				m_np->handleParticipantChanged(m_user, TO_UTF8(nickName()), it->second->getChannel() + m_suffix, pbnetwork::PARTICIPANT_FLAG_CONFLICT);
			}
			m_np->handleDisconnected(m_user, pbnetwork::CONNECTION_ERROR_INVALID_USERNAME, "Nickname collision KILL");
		case 464:
			for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
				m_np->handleParticipantChanged(m_user, TO_UTF8(nickName()), it->second->getChannel() + m_suffix, pbnetwork::PARTICIPANT_FLAG_NOT_AUTHORIZED);
			}
			if (m_suffix.empty()) {
				m_np->handleDisconnected(m_user, pbnetwork::CONNECTION_ERROR_INVALID_USERNAME, "Password incorrect");
			}
		case 321:
			m_rooms.clear();
			m_names.clear();
			break;
		case 322:
			m_rooms.push_back(TO_UTF8(parameters[1]));
			m_names.push_back(TO_UTF8(parameters[1]));
			break;
		case 323:
			m_np->handleRoomList("", m_rooms, m_names);
			break;
		default:
			break;
	}

	if (m->code() >= 400 && m->code() < 500) {
		LOG4CXX_INFO(logger, m_user << ": Error message received: " << message->toData().data());
	}
}

void MyIrcSession::awayTimeout() {
	for(AutoJoinMap::iterator it = m_autoJoin.begin(); it != m_autoJoin.end(); it++) {
		if (it->second->shouldAskWho()) {
			LOG4CXX_INFO(logger, "The time has come. Asking /who " << it->second->getChannel() << " again to get current away states.");
			sendCommand(IrcCommand::createWho(FROM_UTF8(it->second->getChannel())));
		}
	}
}

void MyIrcSession::on_noticeMessageReceived(IrcMessage *message) {
	IrcNoticeMessage *m = (IrcNoticeMessage *) message;
	LOG4CXX_INFO(logger, m_user << ": NOTICE " << TO_UTF8(m->content()));

	QString msg = m->content();
	CommuniBackport::toPlainText(msg);

	std::string target = TO_UTF8(m->target().toLower());
	if (target.find("#") == 0) {
		std::string nickname = TO_UTF8(m->nick());
		correctNickname(nickname);
		m_np->handleMessage(m_user, target + m_suffix, TO_UTF8(msg), nickname);
	}
	else {
		std::string nickname = TO_UTF8(m->nick());
		correctNickname(nickname);
		if (nickname.find(".") != std::string::npos) {
			return;
		}
		if (m_pms.find(nickname) != m_pms.end()) {
			std::string room = m_pms[nickname].substr(0, m_pms[nickname].find("/"));
			room = room.substr(0, room.find("@"));
			if (hasIRCBuddy(room, nickname)) {
				m_np->handleMessage(m_user, room + m_suffix, TO_UTF8(msg), nickname, "", "", false, true);
				return;
			}
			else {
				nickname = nickname + m_suffix;
			}
		}
		else {
			nickname = nickname + m_suffix;
		}

		LOG4CXX_INFO(logger, nickname);
		m_np->handleMessage(m_user, nickname, TO_UTF8(msg), "");
	}
}

void MyIrcSession::onMessageReceived(IrcMessage *message) {
	switch (message->type()) {
		case IrcMessage::Join:
			on_joined(message);
			break;
		case IrcMessage::Part:
			on_parted(message);
			break;
		case IrcMessage::Quit:
			on_quit(message);
			break;
		case IrcMessage::Nick:
			on_nickChanged(message);
			break;
		case IrcMessage::Mode:
			on_modeChanged(message);
			break;
		case IrcMessage::Topic:
			on_topicChanged(message);
			break;
		case IrcMessage::Private:
			on_messageReceived(message);
			break;
		case IrcMessage::Numeric:
			on_numericMessageReceived(message);
			break;
		case IrcMessage::Notice:
			on_noticeMessageReceived(message);
			break;
		case IrcMessage::Whois:
			on_whoisMessageReceived(message);
			break;
		default:break;
	}
}
