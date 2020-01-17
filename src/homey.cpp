/******************************************************************************
 *
 * Copyright (C) 2019 Marton Borzak <hello@martonborzak.com>
 * Copyright (C) 2019 Christian Riedl <ric@rts.co.at>
 * Copyright (C) 2019 Niels de Klerk <hello@martonborzak.com>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#include "homey.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtDebug>

#include "math.h"
#include "yio-interface/entities/blindinterface.h"
#include "yio-interface/entities/lightinterface.h"
#include "yio-interface/entities/mediaplayerinterface.h"

IntegrationInterface::~IntegrationInterface() {}

void HomeyPlugin::create(const QVariantMap &config, EntitiesInterface *entities, NotificationsInterface *notifications,
                         YioAPIInterface *api, ConfigInterface *configObj) {
    QMap<QObject *, QVariant> returnData;

    QVariantList data;
    QString      mdns;

    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "mdns") {
            mdns = iter.value().toString();
        } else if (iter.key() == "data") {
            data = iter.value().toList();
        }
    }

    for (int i = 0; i < data.length(); i++) {
        HomeyBase *ha = new HomeyBase(m_log, this);
        ha->setup(data[i].toMap(), entities, notifications, api, configObj);

        QVariantMap d = data[i].toMap();
        d.insert("mdns", mdns);
        d.insert("type", config.value("type").toString());
        returnData.insert(ha, d);
    }

    emit createDone(returnData);
}

HomeyBase::HomeyBase(QLoggingCategory &log, QObject *parent) : m_log(log) { this->setParent(parent); }

HomeyBase::~HomeyBase() {
    if (m_thread.isRunning()) {
        m_thread.exit();
        m_thread.wait(5000);
    }
}

void HomeyBase::setup(const QVariantMap &config, EntitiesInterface *entities, NotificationsInterface *notifications,
                      YioAPIInterface *api, ConfigInterface *configObj) {
    Integration::setup(config, entities);

    // crate a new instance and pass on variables
    HomeyThread *HAThread = new HomeyThread(config, entities, notifications, api, configObj, m_log);

    // move to thread
    HAThread->moveToThread(&m_thread);

    // connect signals and slots
    QObject::connect(&m_thread, &QThread::finished, HAThread, &QObject::deleteLater);

    QObject::connect(this, &HomeyBase::connectSignal, HAThread, &HomeyThread::connect);
    QObject::connect(this, &HomeyBase::disconnectSignal, HAThread, &HomeyThread::disconnect);
    QObject::connect(this, &HomeyBase::sendCommandSignal, HAThread, &HomeyThread::sendCommand);

    QObject::connect(HAThread, &HomeyThread::stateChanged, this, &HomeyBase::stateHandler);

    m_thread.start();
}

void HomeyBase::connect() { emit connectSignal(); }

void HomeyBase::disconnect() { emit disconnectSignal(); }

void HomeyBase::sendCommand(const QString &type, const QString &entity_id, int command, const QVariant &param) {
    emit sendCommandSignal(type, entity_id, command, param);
}

// FIXME use enum
void HomeyBase::stateHandler(int state) {
    if (state == 0) {
        setState(CONNECTED);
    } else if (state == 1) {
        setState(CONNECTING);
    } else if (state == 2) {
        setState(DISCONNECTED);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// Homey THREAD CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

HomeyThread::HomeyThread(const QVariantMap &config, EntitiesInterface *entities, NotificationsInterface *notifications,
                         YioAPIInterface *api, ConfigInterface *configObj, QLoggingCategory &log)
    : m_notifications(notifications), m_api(api), m_config(configObj), m_log(log) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "data") {
            QVariantMap map = iter.value().toMap();
            m_ip = map.value("ip").toString();
            m_token = map.value("token").toString();
        } else if (iter.key() == "id") {
            m_id = iter.value().toString();
        }
    }
    m_entities = entities;
    // FIXME magic number
    m_webSocketId = 4;

    m_wsReconnectTimer = new QTimer(this);

    m_wsReconnectTimer->setSingleShot(true);
    m_wsReconnectTimer->setInterval(2000);
    m_wsReconnectTimer->stop();

    m_webSocket = new QWebSocket;
    m_webSocket->setParent(this);

    QObject::connect(m_webSocket, SIGNAL(textMessageReceived(const QString &)), this,
                     SLOT(onTextMessageReceived(const QString &)));
    QObject::connect(m_webSocket, SIGNAL(error(QAbstractSocket::SocketError)), this,
                     SLOT(onError(QAbstractSocket::SocketError)));
    QObject::connect(m_webSocket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this,
                     SLOT(onStateChanged(QAbstractSocket::SocketState)));

    QObject::connect(m_wsReconnectTimer, SIGNAL(timeout()), this, SLOT(onTimeout()));
}

void HomeyThread::onTextMessageReceived(const QString &message) {
    QJsonParseError parseerror;
    QJsonDocument   doc = QJsonDocument::fromJson(message.toUtf8(), &parseerror);
    if (parseerror.error != QJsonParseError::NoError) {
        qCCritical(m_log) << "JSON error:" << parseerror.errorString();
        return;
    }
    QVariantMap map = doc.toVariant().toMap();

    QString m = map.value("error").toString();
    if (m.length() > 0) {
        qCCritical(m_log) << "Message error:" << m;
    }

    QString type = map.value("type").toString();
    //    int id = map.value("id").toInt();

    if (type == "connected") {
        setState(IntegrationInterface::CONNECTED);
    }

    // handle get config request from homey app
    if (type == "command" && map.value("command").toString() == "get_config") {
        // get loaded homey entities
        QList<EntityInterface *> es = m_entities->getByIntegration(m_id);

        // create return map object
        QVariantMap returnData;

        // set type
        returnData.insert("type", "sendConfig");

        // create list to store entity ids
        QStringList list;

        // interate throug the list and get the entity ids

        for (EntityInterface *value : es) {
            list.append(value->entity_id());
            qCDebug(m_log) << value->entity_id();
        }
        qCDebug(m_log) << "LIST" << list;
        // insert list to data key in response
        returnData.insert("devices", list);

        // convert map to json
        QJsonDocument doc = QJsonDocument::fromVariant(returnData);
        QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);

        // send message
        m_webSocket->sendTextMessage(message);
    }

    // handle fetch states from homey app
    if (type == "sendStates") {
        QVariantMap data = map.value("data").toMap();
        updateEntity(data.value("entity_id").toString(), data);
    }

    if (type == "event") {
        QVariantMap data = map.value("data").toMap();
        updateEntity(data.value("entity_id").toString(), data);
    }
}

void HomeyThread::onStateChanged(QAbstractSocket::SocketState state) {
    if (state == QAbstractSocket::UnconnectedState && !m_userDisconnect) {
        qCDebug(m_log) << "State changed to 'Unconnected': starting reconnect";
        if (m_webSocket->isValid()) {
            m_webSocket->close();
        }
        setState(IntegrationInterface::DISCONNECTED);
        m_wsReconnectTimer->start();
    }
}

void HomeyThread::onError(QAbstractSocket::SocketError error) {
    qCWarning(m_log) << error << m_webSocket->errorString();
    if (m_webSocket->isValid()) {
        m_webSocket->close();
    }
    setState(IntegrationInterface::DISCONNECTED);
    m_wsReconnectTimer->start();
}

void HomeyThread::onTimeout() {
    if (m_tries == 3) {
        m_wsReconnectTimer->stop();
        qCCritical(m_log) << "Cannot connect to Homey: retried 3 times connecting to" << m_ip;

        QObject *param = this;
        m_notifications->add(
            true, tr("Cannot connect to Homey."), tr("Reconnect"),
            [](QObject *param) {
                Integration *i = qobject_cast<Integration *>(param);
                i->connect();
            },
            param);

        disconnect();
        m_tries = 0;
    } else {
        // FIXME magic number
        m_webSocketId = 4;
        if (m_state != 1) {
            setState(IntegrationInterface::CONNECTING);
        }
        QString url = QString("ws://").append(m_ip);
        qCDebug(m_log) << "Reconnection attempt" << m_tries + 1 << "to Homey server:" << url;
        m_webSocket->open(QUrl(url));

        m_tries++;
    }
}

void HomeyThread::webSocketSendCommand(QVariantMap data) {
    QJsonDocument doc = QJsonDocument::fromVariant(data);
    QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);
    m_webSocket->sendTextMessage(message);
}

int HomeyThread::convertBrightnessToPercentage(float value) { return static_cast<int>(round(value * 100)); }

void HomeyThread::updateEntity(const QString &entity_id, const QVariantMap &attr) {
    EntityInterface *entity = m_entities->getEntityInterface(entity_id);
    if (entity) {
        if (entity->type() == "light") {
            updateLight(entity, attr);
        }
        if (entity->type() == "blind") {
            updateBlind(entity, attr);
        }
        if (entity->type() == "media_player") {
            updateMediaPlayer(entity, attr);
        }
    }
}

void HomeyThread::updateLight(EntityInterface *entity, const QVariantMap &attr) {
    // onoff to state.
    if (attr.contains("onoff")) {
        // attributes.insert("state", attr.value("onoff"));
        entity->setState(attr.value("onoff").toBool() ? LightDef::ON : LightDef::OFF);
        printf("Setting state");
    }

    // brightness
    if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
        if (attr.contains("dim")) {
            // attributes.insert("brightness", convertBrightnessToPercentage(attr.value("dim").toFloat()));
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, convertBrightnessToPercentage(attr.value("dim").toFloat()));
            printf("Setting brightness");
        }
    }

    // color
    if (entity->isSupported(LightDef::F_COLOR)) {
        QVariant     color = attr.value("attributes").toMap().value("rgb_color");
        QVariantList cl(color.toList());
        char         buffer[10];
        snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", cl.value(0).toInt(), cl.value(1).toInt(),
                 cl.value(2).toInt());
        // attributes.insert("color", buffer);
        entity->updateAttrByIndex(LightDef::COLOR, buffer);
    }
}

void HomeyThread::updateBlind(EntityInterface *entity, const QVariantMap &attr) {
    //    QVariantMap attributes;

    //    // state
    //    if (attr.value("state").toString() == "open") {
    //        attributes.insert("state", true);
    //    } else {
    //        attributes.insert("state", false);
    //    }

    //    // position
    //    if (entity->supported_features().indexOf("POSITION") > -1) {
    //        attributes.insert("position", attr.value("attributes").toMap().value("current_position").toInt());
    //    }

    //    m_entities->update(entity->entity_id(), attributes);
}

void HomeyThread::updateMediaPlayer(EntityInterface *entity, const QVariantMap &attr) {
    /*  capabilities:
       [ 'speaker_album',
         'speaker_artist',
         'speaker_duration',
         'speaker_next',
         'speaker_playing',
         'speaker_position',
         'speaker_prev',
         'speaker_repeat',
         'volume_set',
         'volume_mute',
         'speaker_shuffle',
         'speaker_track',
         'sonos_group',
         'sonos_audio_clip' ]
    */
    // QVariantMap attributes;

    // state
    if (attr.contains("speaker_playing")) {
        if (attr.value("speaker_playing").toBool()) {
            // attributes.insert("state", 3); //Playing
            entity->setState(MediaPlayerDef::PLAYING);
            printf("Setting state 2");
        } else {
            // attributes.insert("state", 2); //idle
            entity->setState(MediaPlayerDef::IDLE);
            printf("Setting state 3");
        }
    }

    if (attr.contains("onoff")) {
        if (attr.value("onoff").toBool()) {
            // attributes.insert("state", 1); //On
            entity->setState(MediaPlayerDef::ON);
        } else {
            // attributes.insert("state", 0); //Off
            entity->setState(MediaPlayerDef::OFF);
        }
    }

    // source
    // if (entity->supported_features().indexOf("SOURCE") > -1 && attr.value("attributes").toMap().contains("source")) {
    //    attributes.insert("source", attr.value("attributes").toMap().value("source").toString());
    //}

    // volume  //volume_set
    if (attr.contains("volume_set")) {
        // attributes.insert("volume", int(round(attr.value("volume_set").toDouble()*100)));
        entity->updateAttrByIndex(MediaPlayerDef::VOLUME,
                                  static_cast<int>(round(attr.value("volume_set").toDouble() * 100)));
    }

    // media type
    if (entity->isSupported(MediaPlayerDef::F_MEDIA_TYPE) &&
        attr.value("attributes").toMap().contains("media_content_type")) {
        // attributes.insert("mediaType", attr.value("attributes").toMap().value("media_content_type").toString());
        entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE,
                                  attr.value("attributes").toMap().value("media_content_type").toString());
    }

    // media image
    if (attr.contains("album_art")) {
        // attributes.insert("mediaImage", attr.value("album_art"));
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, attr.value("album_art"));
    }

    // media title
    if (attr.contains("speaker_track")) {
        // attributes.insert("mediaTitle", attr.value("speaker_track").toString());
        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, attr.value("speaker_track").toString());
    }

    // media artist
    if (attr.contains("speaker_artist")) {
        // attributes.insert("mediaArtist", attr.value("speaker_artist").toString());
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, attr.value("speaker_artist").toString());
    }
}

void HomeyThread::setState(int state) {
    m_state = state;
    emit stateChanged(state);
}

void HomeyThread::connect() {
    m_userDisconnect = false;

    setState(IntegrationInterface::CONNECTING);

    // reset the reconnnect trial variable
    m_tries = 0;

    // turn on the websocket connection
    QString url = QString("ws://").append(m_ip);
    qCDebug(m_log) << "Connecting to Homey server:" << url;
    m_webSocket->open(QUrl(url));
}

void HomeyThread::disconnect() {
    m_userDisconnect = true;
    qCDebug(m_log) << "Disconnecting from Homey";

    // turn of the reconnect try
    m_wsReconnectTimer->stop();

    // turn off the socket
    m_webSocket->close();

    setState(IntegrationInterface::DISCONNECTED);
}

void HomeyThread::sendCommand(const QString &type, const QString &entity_id, int command, const QVariant &param) {
    QVariantMap map;
    // example
    // {"command":"onoff","deviceId":"78f3ab16-c622-4bd7-aebf-3ca981e41375","type":"command","value":true}

    QVariantMap attributes;

    map.insert("type", "command");

    map.insert("deviceId", QVariant(entity_id));
    if (type == "light") {
        if (command == LightDef::C_TOGGLE) {
            map.insert("command", QVariant("toggle"));
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_ON) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_OFF) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", false);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_BRIGHTNESS) {
            map.insert("command", "dim");
            float value = param.toFloat() / 100;
            map.insert("value", value);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_COLOR) {
            QColor color = param.value<QColor>();
            // QVariantMap data;
            QVariantList list;
            list.append(color.red());
            list.append(color.green());
            list.append(color.blue());
            map.insert("command", "color");
            map.insert("value", list);
            webSocketSendCommand(map);
            // webSocketSendCommand(type, "turn_on", entity_id, &data);
        }
    }
    if (type == "blind") {
        if (command == BlindDef::C_OPEN) {
            map.insert("command", "windowcoverings_closed");
            map.insert("value", "false");
            webSocketSendCommand(map);
        } else if (command == BlindDef::C_CLOSE) {
            map.insert("command", "windowcoverings_closed");
            map.insert("value", "true");
            webSocketSendCommand(map);
        } else if (command == BlindDef::C_STOP) {
            map.insert("command", "windowcoverings_tilt_set");
            map.insert("value", 0);
            webSocketSendCommand(map);
        } else if (command == BlindDef::C_POSITION) {
            map.insert("command", "windowcoverings_set");
            map.insert("value", param);
            webSocketSendCommand(map);
        }
    }
    if (type == "media_player") {
        if (command == MediaPlayerDef::C_VOLUME_SET) {
            map.insert("command", "volume_set");
            map.insert("value", param.toDouble() / 100);
            attributes.insert("volume", param);
            m_entities->update(entity_id, attributes);  // buggy homey fix
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_PLAY) {
            map.insert("command", "speaker_playing");
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_STOP) {
            map.insert("command", "speaker_playing");
            map.insert("value", false);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_PAUSE) {
            map.insert("command", "speaker_playing");
            map.insert("value", false);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_PREVIOUS) {
            map.insert("command", "speaker_prev");
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_NEXT) {
            map.insert("command", "speaker_next");
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_TURNON) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_TURNOFF) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", false);
            webSocketSendCommand(map);
        }
    }
}
